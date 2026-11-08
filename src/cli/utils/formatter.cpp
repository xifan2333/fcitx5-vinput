#include "cli/utils/formatter.h"

#include <algorithm>
#include <cstddef>
#include <glaze/json/json_t.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unicodelib.h>
#include <unicodelib_encodings.h>
#include <utility>
#include <vector>

#include "common/json/json_access.h"

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

std::string FlattenCellText(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  bool in_whitespace = false;
  for (const char ch : text) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      if (!in_whitespace) {
        result.push_back(' ');
        in_whitespace = true;
      }
    } else {
      result.push_back(ch);
      in_whitespace = false;
    }
  }
  return result;
}

// Matches an ANSI / OSC escape sequence starting at sv.
// Returns the byte length of the matched escape sequence, or 0 if not an escape.
size_t MatchTerminalEscape(std::string_view sv) noexcept {
  if (sv.starts_with("\033[")) {
    // CSI sequence: terminates at characters in ['@'..'~'] (0x40 - 0x7E)
    for (size_t i = 2; i < sv.size(); ++i) {
      const auto ch = static_cast<unsigned char>(sv[i]);
      if (ch >= 0x40 && ch <= 0x7e) {
        return i + 1;
      }
    }
    return sv.size();
  }

  if (sv.starts_with("\033]")) {
    // OSC sequence (e.g. OSC 8 hyperlink): terminates at BEL ('\a') or ST ("\033\\")
    const auto bel_pos = sv.find('\a');
    const auto st_pos = sv.find("\033\\");
    const auto term_pos = std::min(bel_pos, st_pos);
    if (term_pos != std::string_view::npos) {
      return (term_pos == bel_pos) ? term_pos + 1 : term_pos + 2;
    }
    return sv.size();
  }

  return 0;
}

std::string StripInvisibleEscapes(std::string_view input) {
  std::string visible;
  visible.reserve(input.size());
  std::string_view cur = input;

  while (!cur.empty()) {
    if (const auto esc_len = MatchTerminalEscape(cur); esc_len > 0) {
      cur.remove_prefix(esc_len);
    } else {
      visible.push_back(cur.front());
      cur.remove_prefix(1);
    }
  }
  return visible;
}

bool HasUnclosedSgr(std::string_view cell) noexcept {
  const auto last_esc = cell.rfind("\033[");
  if (last_esc == std::string_view::npos) {
    return false;
  }
  return !cell.ends_with("\033[0m") && !cell.ends_with("\033[m");
}

bool HasUnclosedOsc8(std::string_view cell) noexcept {
  const auto last_osc8 = cell.rfind("\033]8;;");
  if (last_osc8 == std::string_view::npos) {
    return false;
  }
  return !cell.ends_with("\033]8;;\a") && !cell.ends_with("\033]8;;\033\\");
}

std::string SanitizeCell(std::string_view raw_text) {
  std::string cell = FlattenCellText(raw_text);
  if (HasUnclosedOsc8(cell)) {
    cell += "\033]8;;\a";
  }
  if (HasUnclosedSgr(cell)) {
    cell += "\033[0m";
  }
  return cell;
}

std::vector<int> ComputeColumnWidths(const std::vector<std::string>& headers,
                                     const std::vector<std::vector<std::string>>& rows) {
  std::vector<int> col_widths(headers.size(), 0);
  for (size_t i = 0; i < headers.size(); ++i) {
    col_widths[i] = MeasureVisibleWidth(headers[i]);
  }
  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size() && i < col_widths.size(); ++i) {
      col_widths[i] = std::max(col_widths[i], MeasureVisibleWidth(row[i]));
    }
  }
  return col_widths;
}

std::string FormatRow(const std::vector<std::string>& cells, const std::vector<int>& col_widths,
                      bool is_header, bool use_color) {
  std::string line;
  for (size_t i = 0; i < col_widths.size(); ++i) {
    if (i > 0) {
      line += "  ";
    }
    const std::string raw = (i < cells.size()) ? cells[i] : "";
    std::string cell = SanitizeCell(raw);

    if (i + 1 < col_widths.size()) {
      cell = PadCellToVisibleWidth(cell, col_widths[i]);
    }

    if (is_header && use_color) {
      line += "\033[1m" + cell + "\033[0m";
    } else {
      line += cell;
    }
  }
  return line;
}

} // namespace

int MeasureVisibleWidth(std::string_view utf8_str) {
  const std::string visible = StripInvisibleEscapes(FlattenCellText(utf8_str));
  const auto u32 = unicode::utf8::decode(visible);
  return unicode::width(u32, unicode::AmbiguousWidth::Narrow);
}

std::string PadCellToVisibleWidth(const std::string& cell, int target_width) {
  std::string safe_cell = SanitizeCell(cell);
  const int w = MeasureVisibleWidth(safe_cell);
  const int pad = target_width - w;
  if (pad <= 0) {
    return safe_cell;
  }
  return safe_cell + std::string(static_cast<size_t>(pad), ' ');
}

std::string RenderTable(const std::vector<std::string>& headers,
                        const std::vector<std::vector<std::string>>& rows, bool use_color) {
  if (headers.empty()) {
    return {};
  }

  const auto col_widths = ComputeColumnWidths(headers, rows);
  std::string output = FormatRow(headers, col_widths, /*is_header=*/true, use_color) + '\n';

  for (const auto& row : rows) {
    output += FormatRow(row, col_widths, /*is_header=*/false, use_color) + '\n';
  }

  return output;
}

} // namespace vinput::cli
