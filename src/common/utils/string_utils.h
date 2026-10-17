#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace vinput::str {

namespace detail {
template <typename T> auto ToCArg(const T& v) {
  if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
    return v.c_str();
  } else {
    return v;
  }
}
} // namespace detail

template <typename... Args> std::string FmtStr(const char* fmt, const Args&... args) {
  int n = std::snprintf(nullptr, 0, fmt, detail::ToCArg(args)...);
  if (n < 0)
    return fmt;
  std::string buf(static_cast<size_t>(n) + 1, '\0');
  std::snprintf(buf.data(), buf.size(), fmt, detail::ToCArg(args)...);
  buf.resize(static_cast<size_t>(n));
  return buf;
}

inline std::string FormatSize(uint64_t bytes) {
  if (bytes >= 1024ULL * 1024 * 1024)
    return FmtStr("%.1f GB", bytes / (1024.0 * 1024 * 1024));
  if (bytes >= 1024ULL * 1024)
    return FmtStr("%.1f MB", bytes / (1024.0 * 1024));
  if (bytes >= 1024)
    return FmtStr("%.1f KB", bytes / 1024.0);
  return FmtStr("%llu B", (unsigned long long)bytes);
}

inline std::string_view TrimAsciiWhitespaceView(std::string_view text) noexcept {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    begin++;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    end--;
  }
  return text.substr(begin, end - begin);
}

inline std::string TrimAsciiWhitespace(std::string_view text) {
  return std::string(TrimAsciiWhitespaceView(text));
}

[[nodiscard]] constexpr std::string_view StripPrefix(std::string_view text,
                                                     std::string_view prefix) noexcept {
  if (text.starts_with(prefix)) {
    return text.substr(prefix.size());
  }
  return text;
}

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

inline std::optional<std::pair<std::string_view, std::string_view>>
SplitPair(std::string_view text, char delimiter, bool trim_whitespace = false) noexcept {
  const size_t pos = text.find(delimiter);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view first = text.substr(0, pos);
  std::string_view second = text.substr(pos + 1);
  if (trim_whitespace) {
    first = TrimAsciiWhitespaceView(first);
    second = TrimAsciiWhitespaceView(second);
  }
  return std::make_pair(first, second);
}

inline std::optional<std::pair<std::string_view, std::string_view>>
SplitPair(std::string_view text, std::string_view delimiter,
          bool trim_whitespace = false) noexcept {
  if (delimiter.empty()) {
    return std::nullopt;
  }
  const size_t pos = text.find(delimiter);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view first = text.substr(0, pos);
  std::string_view second = text.substr(pos + delimiter.size());
  if (trim_whitespace) {
    first = TrimAsciiWhitespaceView(first);
    second = TrimAsciiWhitespaceView(second);
  }
  return std::make_pair(first, second);
}

inline char ToLowerAsciiChar(char ch) noexcept {
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<char>(ch - 'A' + 'a');
  }
  return ch;
}

inline char ToUpperAsciiChar(char ch) noexcept {
  if (ch >= 'a' && ch <= 'z') {
    return static_cast<char>(ch - 'a' + 'A');
  }
  return ch;
}

inline std::string ToLowerAscii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char ch : text) {
    result.push_back(ToLowerAsciiChar(ch));
  }
  return result;
}

inline std::string ToUpperAscii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char ch : text) {
    result.push_back(ToUpperAsciiChar(ch));
  }
  return result;
}

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (ToLowerAsciiChar(a[i]) != ToLowerAsciiChar(b[i])) {
      return false;
    }
  }
  return true;
}

inline std::vector<std::string_view> SplitByChar(std::string_view text, char delim) {
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

inline std::string MaskSecret(std::string_view secret, size_t head_keep = 4, size_t tail_keep = 4,
                              char mask_char = '*') {
  if (secret.empty()) {
    return {};
  }
  if (tail_keep >= secret.size() || head_keep >= secret.size() - tail_keep) {
    std::string masked(secret.size(), mask_char);
    return masked;
  }
  std::string result;
  result.reserve(secret.size());
  result.append(secret.substr(0, head_keep));
  result.append(secret.size() - head_keep - tail_keep, mask_char);
  result.append(secret.substr(secret.size() - tail_keep));
  return result;
}

inline std::string Base64Encode(std::span<const std::byte> data) {
  static constexpr std::string_view kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((data.size() + 2) / 3) * 4);
  for (size_t i = 0; i < data.size(); i += 3) {
    const auto b0 = static_cast<unsigned int>(data[i]);
    const auto b1 = (i + 1 < data.size()) ? static_cast<unsigned int>(data[i + 1]) : 0;
    const auto b2 = (i + 2 < data.size()) ? static_cast<unsigned int>(data[i + 2]) : 0;
    const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
    result.push_back(kTable[(triple >> 18) & 0x3F]);
    result.push_back(kTable[(triple >> 12) & 0x3F]);
    result.push_back((i + 1 < data.size()) ? kTable[(triple >> 6) & 0x3F] : '=');
    result.push_back((i + 2 < data.size()) ? kTable[triple & 0x3F] : '=');
  }
  return result;
}

inline std::string Base64Encode(std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  return Base64Encode(bytes);
}

inline std::string Base64Encode(const char* data, size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  return Base64Encode(std::string_view(data, size));
}

inline std::string Base64Encode(const unsigned char* data, size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  const auto bytes = std::as_bytes(std::span<const unsigned char>(data, size));
  return Base64Encode(bytes);
}

inline bool IsCjkCodepoint(uint32_t cp) {
  return cp >= 0x2E80;
}

inline uint32_t FirstUtf8Codepoint(std::string_view s) {
  if (s.empty())
    return 0;
  auto c = static_cast<unsigned char>(s[0]);
  if (c < 0x80)
    return c;
  uint32_t cp = 0;
  int len = 0;
  if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    len = 2;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    len = 3;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    len = 4;
  } else {
    return c;
  }
  for (int i = 1; i < len && static_cast<size_t>(i) < s.size(); ++i) {
    cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
  }
  return cp;
}

inline uint32_t LastUtf8Codepoint(std::string_view s) {
  if (s.empty())
    return 0;
  auto i = s.size();
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
    --i;
  }
  if (i == 0)
    return 0;
  return FirstUtf8Codepoint(s.substr(i - 1));
}

inline bool IsSentenceEndingPunctuation(uint32_t cp) {
  // CJK sentence enders: 。！？…
  if (cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == 0x2026)
    return true;
  // Latin sentence enders
  if (cp == '.' || cp == '!' || cp == '?')
    return true;
  if (cp == '\n')
    return true;
  return false;
}

inline size_t Utf8CodepointWidth(uint32_t codepoint) {
  if (codepoint == 0) {
    return 0;
  }
  // Control characters & zero-width
  if (codepoint < 32 || (codepoint >= 0x7F && codepoint < 0xA0) || codepoint == 0x200B ||
      codepoint == 0x200C || codepoint == 0x200D || codepoint == 0xFEFF) {
    return 0;
  }
  // CJK, Fullwidth forms, Wide symbols, and Emoji ranges
  if ((codepoint >= 0x1100 && codepoint <= 0x115F) ||   // Hangul Jamo
      (codepoint >= 0x2E80 && codepoint <= 0x9FFF) ||   // CJK Radicals, Punctuation, Ideographs
      (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||   // Hangul Syllables
      (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||   // CJK Compatibility Ideographs
      (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||   // Vertical forms
      (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||   // CJK Compatibility Forms
      (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||   // Fullwidth Forms
      (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) ||   // Fullwidth Symbols
      (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) || // Miscellaneous Symbols & Emojis
      (codepoint >= 0x20000 && codepoint <= 0x2FA1F) || // CJK Unified Ideographs Extension
      (codepoint >= 0x30000 && codepoint <= 0x3134F)) {
    return 2;
  }
  return 1;
}

inline uint32_t NextUtf8Codepoint(std::string_view text, size_t& offset) {
  if (offset >= text.size()) {
    return 0;
  }
  const auto c = static_cast<unsigned char>(text[offset]);
  uint32_t codepoint = 0;
  size_t len = 1;
  if (c < 0x80) {
    codepoint = c;
    len = 1;
  } else if ((c & 0xE0) == 0xC0) {
    codepoint = c & 0x1F;
    len = 2;
  } else if ((c & 0xF0) == 0xE0) {
    codepoint = c & 0x0F;
    len = 3;
  } else if ((c & 0xF8) == 0xF0) {
    codepoint = c & 0x07;
    len = 4;
  } else {
    offset += 1;
    return c;
  }
  for (size_t i = 1; i < len && offset + i < text.size(); ++i) {
    codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[offset + i]) & 0x3F);
  }
  offset += len;
  return codepoint;
}

inline size_t Utf8VisualWidth(std::string_view text) {
  size_t width = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    const uint32_t codepoint = NextUtf8Codepoint(text, offset);
    width += Utf8CodepointWidth(codepoint);
  }
  return width;
}

inline std::string TruncateMiddleUtf8(std::string_view text, size_t max_width,
                                      std::string_view ellipsis = "...") {
  if (max_width == 0) {
    return std::string(text);
  }

  const size_t total_width = Utf8VisualWidth(text);
  if (total_width <= max_width) {
    return std::string(text);
  }

  const size_t ellipsis_width = Utf8VisualWidth(ellipsis);
  if (max_width <= ellipsis_width + 4) {
    const size_t tail_budget = max_width;
    size_t index = text.size();
    size_t current_width = 0;
    while (index > 0) {
      size_t prev_index = index - 1;
      while (prev_index > 0 && (static_cast<unsigned char>(text[prev_index]) & 0xC0) == 0x80) {
        --prev_index;
      }
      const uint32_t codepoint = FirstUtf8Codepoint(text.substr(prev_index));
      const size_t cp_width = Utf8CodepointWidth(codepoint);
      if (current_width + cp_width > tail_budget) {
        break;
      }
      current_width += cp_width;
      index = prev_index;
    }
    return std::string(text.substr(index));
  }

  const size_t available_content_width = max_width - ellipsis_width;
  size_t target_head_width = std::max<size_t>(6, std::min<size_t>(14, available_content_width / 4));
  if (target_head_width >= available_content_width) {
    target_head_width = available_content_width / 2;
  }
  const size_t target_tail_width = available_content_width - target_head_width;

  size_t head_byte_len = 0;
  size_t head_cur_width = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    size_t next_offset = offset;
    const uint32_t codepoint = NextUtf8Codepoint(text, next_offset);
    const size_t cp_width = Utf8CodepointWidth(codepoint);
    if (head_cur_width + cp_width > target_head_width) {
      break;
    }
    head_cur_width += cp_width;
    offset = next_offset;
    head_byte_len = offset;
  }

  size_t tail_start_offset = text.size();
  size_t tail_cur_width = 0;
  size_t index = text.size();
  while (index > head_byte_len) {
    size_t prev_index = index - 1;
    while (prev_index > 0 && (static_cast<unsigned char>(text[prev_index]) & 0xC0) == 0x80) {
      --prev_index;
    }
    const uint32_t codepoint = FirstUtf8Codepoint(text.substr(prev_index));
    const size_t cp_width = Utf8CodepointWidth(codepoint);
    if (tail_cur_width + cp_width > target_tail_width) {
      break;
    }
    tail_cur_width += cp_width;
    index = prev_index;
    tail_start_offset = index;
  }

  std::string result;
  result.reserve(head_byte_len + ellipsis.size() + (text.size() - tail_start_offset));
  result.append(text.substr(0, head_byte_len));
  result.append(ellipsis);
  result.append(text.substr(tail_start_offset));
  return result;
}

} // namespace vinput::str
