#include "cli/utils/formatter.h"

#include <algorithm>
#include <cstddef>
#include <glaze/json/json_t.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unicodelib.h>
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
  bool in_newline = false;
  for (const char ch : text) {
    if (ch == '\r' || ch == '\n') {
      if (!in_newline) {
        result.push_back(' ');
        in_newline = true;
      }
    } else {
      result.push_back(ch);
      in_newline = false;
    }
  }
  return result;
}

bool HasUnclosedSgr(std::string_view cell) {
  const auto last_esc = cell.rfind("\033[");
  if (last_esc == std::string_view::npos) {
    return false;
  }
  return !cell.ends_with("\033[0m") && !cell.ends_with("\033[m");
}

} // namespace

int MeasureVisibleWidth(std::string_view utf8_str) {
  const std::string flat = FlattenCellText(utf8_str);
  std::u32string visible_cps;
  visible_cps.reserve(flat.size());

  const auto is_cont = [](unsigned char b) noexcept { return (b & 0xc0) == 0x80; };

  for (size_t i = 0; i < flat.size();) {
    if (flat[i] == '\033') {
      if (i + 1 < flat.size() && flat[i + 1] == ']') {
        // OSC sequence: \033] ... (\a or \033\\)
        i += 2;
        while (i < flat.size()) {
          if (flat[i] == '\a') {
            ++i;
            break;
          }
          if (flat[i] == '\033' && i + 1 < flat.size() && flat[i + 1] == '\\') {
            i += 2;
            break;
          }
          ++i;
        }
        continue;
      }
      if (i + 1 < flat.size() && flat[i + 1] == '[') {
        // CSI / SGR sequence: \033[ ... [0x40-0x7e]
        i += 2;
        while (i < flat.size()) {
          const auto ch = static_cast<unsigned char>(flat[i]);
          ++i;
          if (ch >= 0x40 && ch <= 0x7e) {
            break;
          }
        }
        continue;
      }
    }

    const auto c = static_cast<unsigned char>(flat[i]);
    char32_t cp = 0;
    size_t bytes = 0;
    if (c < 0x80) {
      cp = c;
      bytes = 1;
    } else if ((c & 0xe0) == 0xc0) {
      if (i + 1 < flat.size() && is_cont(static_cast<unsigned char>(flat[i + 1]))) {
        cp = static_cast<char32_t>(((c & 0x1f) << 6) |
                                   (static_cast<unsigned char>(flat[i + 1]) & 0x3f));
        bytes = 2;
      }
    } else if ((c & 0xf0) == 0xe0) {
      if (i + 2 < flat.size() && is_cont(static_cast<unsigned char>(flat[i + 1])) &&
          is_cont(static_cast<unsigned char>(flat[i + 2]))) {
        cp = static_cast<char32_t>(((c & 0x0f) << 12) |
                                   ((static_cast<unsigned char>(flat[i + 1]) & 0x3f) << 6) |
                                   (static_cast<unsigned char>(flat[i + 2]) & 0x3f));
        bytes = 3;
      }
    } else if ((c & 0xf8) == 0xf0) {
      if (i + 3 < flat.size() && is_cont(static_cast<unsigned char>(flat[i + 1])) &&
          is_cont(static_cast<unsigned char>(flat[i + 2])) &&
          is_cont(static_cast<unsigned char>(flat[i + 3]))) {
        cp = static_cast<char32_t>(((c & 0x07) << 18) |
                                   ((static_cast<unsigned char>(flat[i + 1]) & 0x3f) << 12) |
                                   ((static_cast<unsigned char>(flat[i + 2]) & 0x3f) << 6) |
                                   (static_cast<unsigned char>(flat[i + 3]) & 0x3f));
        bytes = 4;
      }
    }

    if (bytes > 0) {
      visible_cps.push_back(cp);
      i += bytes;
    } else {
      // Fallback: invalid UTF-8 byte, advance 1 and count as replacement char (width 1)
      visible_cps.push_back(0xFFFD);
      ++i;
    }
  }

  return unicode::width(visible_cps, unicode::AmbiguousWidth::Narrow);
}

std::string PadCellToVisibleWidth(const std::string& cell, int target_width) {
  std::string safe_cell = FlattenCellText(cell);
  if (HasUnclosedSgr(safe_cell)) {
    safe_cell += "\033[0m";
  }
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

  std::vector<int> col_widths(headers.size(), 0);
  for (size_t i = 0; i < headers.size(); ++i) {
    col_widths[i] = MeasureVisibleWidth(headers[i]);
  }
  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size() && i < col_widths.size(); ++i) {
      col_widths[i] = std::max(col_widths[i], MeasureVisibleWidth(row[i]));
    }
  }

  std::string output;

  // Header row
  for (size_t i = 0; i < headers.size(); ++i) {
    if (i > 0) {
      output += "  ";
    }
    const std::string cell =
        (i + 1 < headers.size()) ? PadCellToVisibleWidth(headers[i], col_widths[i]) : headers[i];
    if (use_color) {
      output += "\033[1m" + cell + "\033[0m";
    } else {
      output += cell;
    }
  }
  output += '\n';

  // Data rows
  for (const auto& row : rows) {
    for (size_t i = 0; i < headers.size(); ++i) {
      if (i > 0) {
        output += "  ";
      }
      const std::string raw_cell = (i < row.size()) ? row[i] : "";
      std::string cell = FlattenCellText(raw_cell);
      if (HasUnclosedSgr(cell)) {
        cell += "\033[0m";
      }
      if (i + 1 < headers.size()) {
        output += PadCellToVisibleWidth(cell, col_widths[i]);
      } else {
        output += cell;
      }
    }
    output += '\n';
  }

  return output;
}
}

} // namespace vinput::cli
