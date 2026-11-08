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

// ---- TextFormatter ----

TextFormatter::TextFormatter(bool use_color) : use_color_(use_color) {}

std::string TextFormatter::Green(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[32m" + s + "\033[0m";
}

std::string TextFormatter::Red(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[31m" + s + "\033[0m";
}

std::string TextFormatter::Yellow(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[33m" + s + "\033[0m";
}

std::string TextFormatter::Gray(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[90m" + s + "\033[0m";
}

std::string TextFormatter::Bold(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[1m" + s + "\033[0m";
}

void TextFormatter::PrintTable(const std::vector<std::string>& headers,
                               const std::vector<std::vector<std::string>>& rows) {
  std::cout << vinput::cli::RenderTable(headers, rows, use_color_);
}

void TextFormatter::PrintKeyValue(const std::string& key, const std::string& value) {
  std::cout << Bold(key) << ": " << value << "\n";
}

void TextFormatter::PrintSuccess(const std::string& msg) {
  std::cout << Green(msg) << "\n";
}

void TextFormatter::PrintError(const std::string& msg) {
  std::cerr << Red(msg) << "\n";
}

void TextFormatter::PrintWarning(const std::string& msg) {
  std::cout << Yellow(msg) << "\n";
}

void TextFormatter::PrintJson(const glz::json_t& j) {
  std::cout << vinput::json::ToPrettyText(j) << "\n";
}

void TextFormatter::PrintInfo(const std::string& msg) {
  std::cout << Gray(msg) << "\n";
}

// ---- JsonFormatter ----

void JsonFormatter::PrintTable(const std::vector<std::string>& headers,
                               const std::vector<std::vector<std::string>>& rows) {
  glz::json_t::array_t arr;
  arr.reserve(rows.size());
  for (const auto& row : rows) {
    glz::json_t::object_t obj;
    for (size_t i = 0; i < headers.size(); ++i) {
      obj.emplace(headers[i], glz::json_t((i < row.size()) ? row[i] : std::string{}));
    }
    arr.emplace_back(std::move(obj));
  }
  std::cout << vinput::json::ToPrettyText(glz::json_t(std::move(arr))) << "\n";
}

void JsonFormatter::PrintKeyValue(const std::string& key, const std::string& value) {
  const glz::json_t obj = {{"key", key}, {"value", value}};
  std::cout << vinput::json::ToText(obj) << "\n";
}

void JsonFormatter::PrintSuccess(const std::string& msg) {
  const glz::json_t obj = {{"status", "success"}, {"message", msg}};
  std::cout << vinput::json::ToText(obj) << "\n";
}

void JsonFormatter::PrintError(const std::string& msg) {
  const glz::json_t obj = {{"status", "error"}, {"message", msg}};
  std::cerr << vinput::json::ToText(obj) << "\n";
}

void JsonFormatter::PrintWarning(const std::string& msg) {
  const glz::json_t obj = {{"status", "warning"}, {"message", msg}};
  std::cout << vinput::json::ToText(obj) << "\n";
}

void JsonFormatter::PrintJson(const glz::json_t& j) {
  std::cout << vinput::json::ToPrettyText(j) << "\n";
}

void JsonFormatter::PrintInfo(const std::string& msg) {
  const glz::json_t obj = {{"status", "info"}, {"message", msg}};
  std::cout << vinput::json::ToText(obj) << "\n";
}

// ---- Factory ----

std::unique_ptr<Formatter> CreateFormatter(const CliContext& ctx) {
  if (ctx.json_output) {
    return std::make_unique<JsonFormatter>();
  }
  return std::make_unique<TextFormatter>(ctx.is_tty);
}

namespace vinput::cli {

namespace {

struct TerminalState {
  bool in_hyperlink{false};
  bool active_sgr{false};
};

TerminalState InspectTerminalState(std::string_view text) noexcept {
  TerminalState state{};
  std::string_view cur = text;

  while (!cur.empty()) {
    const auto esc_pos = cur.find('\033');
    if (esc_pos == std::string_view::npos) {
      break;
    }
    cur.remove_prefix(esc_pos);

    if (cur.starts_with("\033[")) {
      size_t end_idx = 2;
      while (end_idx < cur.size()) {
        const auto ch = static_cast<unsigned char>(cur[end_idx++]);
        if (ch >= 0x40 && ch <= 0x7e) {
          if (ch == 'm') {
            const std::string_view params = cur.substr(2, end_idx - 3);
            state.active_sgr = !(params.empty() || params == "0" || params == "00");
          }
          break;
        }
      }
      cur.remove_prefix(end_idx);
    } else if (cur.starts_with("\033]")) {
      const auto bel_pos = cur.find('\a');
      const auto st_pos = cur.find("\033\\");
      const auto term_pos = std::min(bel_pos, st_pos);
      if (term_pos == std::string_view::npos) {
        if (cur.starts_with("\033]8;")) {
          state.in_hyperlink = true;
        }
        break;
      }

      if (cur.starts_with("\033]8;")) {
        const auto payload = cur.substr(0, term_pos);
        state.in_hyperlink = !(payload == "\033]8;;" || payload.ends_with(";"));
      }
      cur.remove_prefix((term_pos == bel_pos) ? term_pos + 1 : term_pos + 2);
    } else {
      cur.remove_prefix(1);
    }
  }

  return state;
}

std::string SanitizeCell(std::string_view raw_text) {
  std::string cell = vinput::str::FlattenLineBreaks(raw_text);
  const auto state = InspectTerminalState(cell);
  if (state.in_hyperlink) {
    cell += "\033]8;;\a";
  }
  if (state.active_sgr) {
    cell += "\033[0m";
  }
  return cell;
}

std::vector<int> ComputeColumnWidths(const std::vector<std::string>& headers,
                                     const std::vector<std::vector<std::string>>& rows) {
  std::vector<int> col_widths;
  col_widths.reserve(headers.size());

  for (const auto& header : headers) {
    col_widths.push_back(MeasureVisibleWidth(SanitizeCell(header)));
  }

  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size() && i < col_widths.size(); ++i) {
      col_widths[i] = std::max(col_widths[i], MeasureVisibleWidth(SanitizeCell(row[i])));
    }
  }
  return col_widths;
}

std::string FormatRow(const std::vector<std::string>& cells, const std::vector<int>& col_widths) {
  std::vector<std::string> formatted_cells;
  formatted_cells.reserve(col_widths.size());

  for (size_t i = 0; i < col_widths.size(); ++i) {
    const std::string raw = (i < cells.size()) ? cells[i] : "";
    std::string cell = SanitizeCell(raw);

    if (i + 1 < col_widths.size()) {
      cell = PadCellToVisibleWidth(cell, col_widths[i]);
    }
    formatted_cells.push_back(std::move(cell));
  }
  return vinput::str::Join(formatted_cells, "  ");
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

  const auto col_widths = ComputeColumnWidths(headers, rows);
  std::vector<std::string> lines;
  lines.reserve(rows.size() + 1);

  std::string header_line = FormatRow(headers, col_widths);
  if (use_color) {
    header_line = "\033[1m" + header_line + "\033[0m";
  }
  lines.push_back(std::move(header_line));

  for (const auto& row : rows) {
    lines.push_back(FormatRow(row, col_widths));
  }

  return vinput::str::Join(lines, "\n") + '\n';
}

} // namespace vinput::cli
