#include "cli/utils/formatter.h"

#include <algorithm>
#include <cstddef>
#include <glaze/json/json_t.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/json/json_access.h"
#include "common/utils/string_utils.h"

#include "cli/utils/cli_context.h"

// Machine readable output keeps the JSON contract of the CLI: `PrintJson` writes
// two space indented output with explicit nulls, while the single value helpers
// stay compact.

namespace {

class TextFormatter final : public Formatter {
public:
  explicit TextFormatter(bool use_color) : use_color_(use_color) {}

  void PrintTable(const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows) override {
    std::cout << vinput::cli::RenderTable(headers, rows, use_color_);
  }
  void PrintKeyValue(const std::string& key, const std::string& value) override {
    std::cout << Bold(key) << ": " << value << "\n";
  }
  void PrintSuccess(const std::string& msg) override { std::cout << Green(msg) << "\n"; }
  void PrintError(const std::string& msg) override { std::cerr << Red(msg) << "\n"; }
  void PrintWarning(const std::string& msg) override { std::cout << Yellow(msg) << "\n"; }
  void PrintJson(const glz::json_t& j) override {
    std::cout << vinput::json::ToPrettyText(j) << "\n";
  }
  void PrintInfo(const std::string& msg) override { std::cout << Gray(msg) << "\n"; }

private:
  bool use_color_;
  [[nodiscard]] std::string Green(const std::string& s) const {
    return use_color_ ? ("\033[32m" + s + "\033[0m") : s;
  }
  [[nodiscard]] std::string Red(const std::string& s) const {
    return use_color_ ? ("\033[31m" + s + "\033[0m") : s;
  }
  [[nodiscard]] std::string Yellow(const std::string& s) const {
    return use_color_ ? ("\033[33m" + s + "\033[0m") : s;
  }
  [[nodiscard]] std::string Gray(const std::string& s) const {
    return use_color_ ? ("\033[90m" + s + "\033[0m") : s;
  }
  [[nodiscard]] std::string Bold(const std::string& s) const {
    return use_color_ ? ("\033[1m" + s + "\033[0m") : s;
  }
};

class JsonFormatter final : public Formatter {
public:
  void PrintTable(const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows) override {
    glz::json_t::array_t arr;
    arr.reserve(rows.size());
    for (const auto& row : rows) {
      arr.emplace_back(RowToJsonObject(headers, row));
    }
    std::cout << vinput::json::ToPrettyText(glz::json_t(std::move(arr))) << "\n";
  }

  void PrintKeyValue(const std::string& key, const std::string& value) override {
    const glz::json_t obj = {{"key", key}, {"value", value}};
    PrintJson(obj, /*pretty=*/false);
  }
  void PrintSuccess(const std::string& msg) override { PrintStatus("success", msg); }
  void PrintError(const std::string& msg) override {
    PrintStatus("error", msg, /*is_stderr=*/true);
  }
  void PrintWarning(const std::string& msg) override { PrintStatus("warning", msg); }
  void PrintInfo(const std::string& msg) override { PrintStatus("info", msg); }
  void PrintJson(const glz::json_t& j) override { PrintJson(j, /*pretty=*/true); }

private:
  static glz::json_t::object_t RowToJsonObject(const std::vector<std::string>& headers,
                                               const std::vector<std::string>& row) {
    glz::json_t::object_t obj;
    for (size_t i = 0; i < headers.size(); ++i) {
      obj.emplace(headers[i], glz::json_t((i < row.size()) ? row[i] : std::string{}));
    }
    return obj;
  }

  static void PrintJson(const glz::json_t& j, bool pretty) {
    if (pretty) {
      std::cout << vinput::json::ToPrettyText(j) << "\n";
    } else {
      std::cout << vinput::json::ToText(j) << "\n";
    }
  }

  static void PrintStatus(const std::string& status, const std::string& msg,
                          bool is_stderr = false) {
    const glz::json_t obj = {{"status", status}, {"message", msg}};
    if (is_stderr) {
      std::cerr << vinput::json::ToText(obj) << "\n";
    } else {
      std::cout << vinput::json::ToText(obj) << "\n";
    }
  }
};

} // namespace

// ---- Factory ----

std::unique_ptr<Formatter> CreateFormatter(const CliContext& ctx) {
  if (ctx.json_output) {
    return std::make_unique<JsonFormatter>();
  }
  return std::make_unique<TextFormatter>(ctx.is_tty);
}

namespace vinput::cli {

namespace {

using Cell = vinput::str::ProcessedTerminalCell;

struct TableGrid {
  std::vector<Cell> headers;
  std::vector<std::vector<Cell>> rows;
  std::vector<int> col_widths;
};

std::vector<Cell> ProcessGridRow(const std::vector<std::string>& row,
                                 std::vector<int>& col_widths) {
  std::vector<Cell> processed_row;
  processed_row.reserve(row.size());
  for (size_t i = 0; i < row.size(); ++i) {
    auto cell = vinput::str::SanitizeTerminalCell(row[i]);
    if (i < col_widths.size()) {
      col_widths[i] = std::max(col_widths[i], cell.visible_width);
    }
    processed_row.push_back(std::move(cell));
  }
  return processed_row;
}

TableGrid BuildTableGrid(const std::vector<std::string>& headers,
                         const std::vector<std::vector<std::string>>& rows) {
  TableGrid grid{};
  grid.headers.reserve(headers.size());
  grid.col_widths.reserve(headers.size());

  for (const auto& header : headers) {
    auto cell = vinput::str::SanitizeTerminalCell(header);
    grid.col_widths.push_back(cell.visible_width);
    grid.headers.push_back(std::move(cell));
  }

  grid.rows.reserve(rows.size());
  for (const auto& row : rows) {
    grid.rows.push_back(ProcessGridRow(row, grid.col_widths));
  }

  return grid;
}

std::string FormatAlignedRow(const std::vector<Cell>& cells, const std::vector<int>& col_widths) {
  std::vector<std::string> parts;
  parts.reserve(col_widths.size());

  for (size_t i = 0; i < col_widths.size(); ++i) {
    std::string text = (i < cells.size()) ? cells[i].sanitized_text : std::string{};
    const int w = (i < cells.size()) ? cells[i].visible_width : 0;
    if (i + 1 < col_widths.size() && col_widths[i] > w) {
      text.append(static_cast<size_t>(col_widths[i] - w), ' ');
    }
    parts.push_back(std::move(text));
  }

  return vinput::str::Join(parts, "  ");
}

} // namespace

int MeasureVisibleWidth(std::string_view utf8_str) {
  return vinput::str::DisplayWidth(utf8_str);
}

std::string PadCellToVisibleWidth(const std::string& cell, int target_width) {
  const int w = MeasureVisibleWidth(cell);
  const int pad = target_width - w;
  if (pad <= 0) {
    return cell;
  }
  return cell + std::string(static_cast<size_t>(pad), ' ');
}

std::string RenderTable(const std::vector<std::string>& headers,
                        const std::vector<std::vector<std::string>>& rows, bool use_color) {
  if (headers.empty()) {
    return {};
  }

  const auto grid = BuildTableGrid(headers, rows);
  std::vector<std::string> lines;
  lines.reserve(grid.rows.size() + 1);

  std::string header_line = FormatAlignedRow(grid.headers, grid.col_widths);
  if (use_color) {
    header_line = "\033[1m" + header_line + "\033[0m";
  }
  lines.push_back(std::move(header_line));

  for (const auto& row : grid.rows) {
    lines.push_back(FormatAlignedRow(row, grid.col_widths));
  }

  return vinput::str::Join(lines, "\n") + '\n';
}

} // namespace vinput::cli
