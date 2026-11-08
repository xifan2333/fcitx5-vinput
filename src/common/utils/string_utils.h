#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <unicodelib.h>
#include <unicodelib_encodings.h>
#include <utility>
#include <vector>

namespace vinput::str {

// =========================================================================
// 1. Trimming (Zero-Copy)
// =========================================================================

[[nodiscard]] inline std::string_view Trim(std::string_view text) noexcept {
  const auto not_ws = [](char c) noexcept -> bool {
    return std::isspace(static_cast<unsigned char>(c)) == 0;
  };
  const auto* const start = std::ranges::find_if(text, not_ws);
  if (start == text.end()) {
    return {};
  }
  const auto rstart = std::ranges::find_if(text | std::views::reverse, not_ws);
  const auto begin_idx = static_cast<size_t>(start - text.begin());
  const auto end_idx = static_cast<size_t>(text.rend() - rstart);
  return text.substr(begin_idx, end_idx - begin_idx);
}

// =========================================================================
// 3. Affixes (Prefix & Suffix)
// =========================================================================

[[nodiscard]] constexpr std::string_view StripSuffix(std::string_view text,
                                                     std::string_view suffix) noexcept {
  if (text.ends_with(suffix)) {
    return text.substr(0, text.size() - suffix.size());
  }
  return text;
}

constexpr bool ConsumePrefix(std::string_view* text, std::string_view prefix) noexcept {
  if (text != nullptr && text->starts_with(prefix)) {
    text->remove_prefix(prefix.size());
    return true;
  }
  return false;
}

// =========================================================================
// 4. Splitting
// =========================================================================

inline std::vector<std::string_view> Split(std::string_view text, char delim) {
  std::vector<std::string_view> tokens;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find(delim, start);
    if (end == std::string_view::npos) {
      tokens.push_back(text.substr(start));
      break;
    }
    tokens.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return tokens;
}

inline std::vector<std::string_view> SplitLines(std::string_view text) {
  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    size_t len = end - start;
    if (len > 0 && text[start + len - 1] == '\r') {
      --len;
    }
    lines.push_back(text.substr(start, len));
    start = end + 1;
  }
  return lines;
}

inline std::optional<std::pair<std::string_view, std::string_view>>
SplitOnce(std::string_view text, char delimiter) noexcept {
  const size_t pos = text.find(delimiter);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  return std::make_pair(text.substr(0, pos), text.substr(pos + 1));
}

inline std::optional<std::pair<std::string_view, std::string_view>>
RSplitOnce(std::string_view text, char delimiter) noexcept {
  const size_t pos = text.rfind(delimiter);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  return std::make_pair(text.substr(0, pos), text.substr(pos + 1));
}

// =========================================================================
// 5. Joining
// =========================================================================

template <typename Container>
inline std::string Join(const Container& elements, std::string_view delimiter) {
  auto it = std::begin(elements);
  const auto end = std::end(elements);
  if (it == end) {
    return {};
  }

  size_t total_size = 0;
  size_t count = 0;
  for (auto cur = it; cur != end; ++cur) {
    total_size += std::string_view(*cur).size();
    ++count;
  }
  if (count > 1) {
    total_size += (count - 1) * delimiter.size();
  }

  std::string result;
  result.reserve(total_size);

  result.append(std::string_view(*it));
  ++it;
  for (; it != end; ++it) {
    result.append(delimiter);
    result.append(std::string_view(*it));
  }
  return result;
}

inline std::string Join(std::initializer_list<std::string_view> elements,
                        std::string_view delimiter) {
  return Join<std::initializer_list<std::string_view>>(elements, delimiter);
}

template <typename MapContainer>
inline std::string JoinKeyValue(const MapContainer& map, std::string_view item_delimiter,
                                std::string_view kv_delimiter) {
  auto it = std::begin(map);
  const auto end = std::end(map);
  if (it == end) {
    return {};
  }

  size_t total_size = 0;
  size_t count = 0;
  for (auto cur = it; cur != end; ++cur) {
    total_size += std::string_view(cur->first).size() + kv_delimiter.size() +
                  std::string_view(cur->second).size();
    ++count;
  }
  if (count > 1) {
    total_size += (count - 1) * item_delimiter.size();
  }

  std::string result;
  result.reserve(total_size);

  auto append_entry = [&](const auto& pair) {
    result.append(std::string_view(pair.first));
    result.append(kv_delimiter);
    result.append(std::string_view(pair.second));
  };

  append_entry(*it);
  ++it;
  for (; it != end; ++it) {
    result.append(item_delimiter);
    append_entry(*it);
  }
  return result;
}

// =========================================================================
// 6. Comparison (Case-Insensitive)
// =========================================================================

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
  return std::ranges::equal(a, b, [](char ca, char cb) noexcept {
    return std::tolower(static_cast<unsigned char>(ca)) ==
           std::tolower(static_cast<unsigned char>(cb));
  });
}

[[nodiscard]] inline bool StartsWithIgnoreCase(std::string_view text,
                                               std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

// =========================================================================
// 7. Numeric Parsing
// =========================================================================

// Parses the whole trimmed string as an integer in the given base. Returns
// nullopt for empty input, malformed digits, or trailing characters.
template <typename T> inline std::optional<T> ParseInteger(std::string_view text, int base = 10) {
  const std::string_view trimmed = Trim(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  T parsed{};
  const char* begin = trimmed.data();
  const char* end = begin + trimmed.size();
  const auto result = std::from_chars(begin, end, parsed, base);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

// =========================================================================
// 8. Terminal Text & Escapes
// =========================================================================

// Strips terminal escape sequences (ANSI CSI/SGR styles and OSC 8 hyperlinks),
// leaving only visible UTF-8 text via chunk-based navigation.
[[nodiscard]] inline std::string StripTerminalEscapes(std::string_view text) {
  std::string visible;
  visible.reserve(text.size());
  std::string_view cur = text;

  while (!cur.empty()) {
    const auto esc_pos = cur.find('\033');
    if (esc_pos == std::string_view::npos) {
      visible.append(cur);
      break;
    }

    if (esc_pos > 0) {
      visible.append(cur.substr(0, esc_pos));
      cur.remove_prefix(esc_pos);
    }

    if (cur.starts_with("\033[")) {
      // CSI / SGR: terminates at characters in ['@'..'~'] (0x40 - 0x7E)
      size_t end_idx = 2;
      while (end_idx < cur.size()) {
        const auto ch = static_cast<unsigned char>(cur[end_idx++]);
        if (ch >= 0x40 && ch <= 0x7e) {
          break;
        }
      }
      cur.remove_prefix(end_idx);
    } else if (cur.starts_with("\033]")) {
      // OSC (e.g. OSC 8 hyperlink): terminates at BEL ('\a') or ST ("\033\\")
      const auto bel_pos = cur.find('\a');
      const auto st_pos = cur.find("\033\\");
      const auto term_pos = std::min(bel_pos, st_pos);
      if (term_pos != std::string_view::npos) {
        cur.remove_prefix((term_pos == bel_pos) ? term_pos + 1 : term_pos + 2);
      } else {
        break;
      }
    } else {
      cur.remove_prefix(1);
    }
  }

  return visible;
}

// Calculates visual display column width under Unicode UAX #11, correctly
// discounting terminal escape sequences and measuring East Asian Wide (CJK) as 2.
[[nodiscard]] inline int DisplayWidth(std::string_view text) {
  const std::string visible = StripTerminalEscapes(text);
  const auto u32 = unicode::utf8::decode(visible);
  return unicode::width(u32, unicode::AmbiguousWidth::Narrow);
}

// Flattens runs of carriage returns, newlines, and tabs into a single space,
// preserving intentional indentation and spaces in the original text.
[[nodiscard]] inline std::string FlattenLineBreaks(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  std::string_view cur = text;

  while (!cur.empty()) {
    const auto break_pos = cur.find_first_of("\r\n\t");
    if (break_pos == std::string_view::npos) {
      result.append(cur);
      break;
    }

    result.append(cur.substr(0, break_pos));
    result.push_back(' ');
    cur.remove_prefix(break_pos);

    const auto next_valid = cur.find_first_not_of("\r\n\t");
    if (next_valid == std::string_view::npos) {
      break;
    }
    cur.remove_prefix(next_valid);
  }

  return result;
}

// Collapses runs of whitespace, carriage returns, newlines, and tabs into a single space.
[[nodiscard]] inline std::string CollapseWhitespace(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  bool in_whitespace = false;
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
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

} // namespace vinput::str
