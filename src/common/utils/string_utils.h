#pragma once
#include <algorithm>
#include <base64.hpp>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unicodelib.h>
#include <unicodelib_encodings.h>
#include <utility>
#include <vector>

namespace vinput::str {

namespace detail {

template <typename T> auto PrepareStorage(const T& v) {
  using RawT = std::decay_t<T>;
  if constexpr (std::is_same_v<RawT, std::string_view>) {
    return v.empty() ? std::string{} : std::string(v);
  } else {
    return std::cref(v);
  }
}

template <typename T> auto ToCArg(const T& v) {
  using RawT = std::decay_t<T>;
  if constexpr (std::is_same_v<RawT, std::string>) {
    return v.c_str();
  } else {
    static_assert(!std::is_same_v<RawT, std::string_view>,
                  "std::string_view must be adapted via PrepareStorage before snprintf");
    return v;
  }
}

template <typename T> auto ToCArg(const std::reference_wrapper<T>& ref) {
  return ToCArg(ref.get());
}

} // namespace detail

template <typename... Args> std::string FmtStr(const char* fmt, const Args&... args) {
  if (fmt == nullptr) {
    return {};
  }
  const auto storage = std::make_tuple(detail::PrepareStorage(args)...);
  return std::apply(
      [fmt](const auto&... items) {
        const int n = std::snprintf(nullptr, 0, fmt, detail::ToCArg(items)...);
        if (n < 0) {
          return std::string(fmt);
        }
        std::string buf(static_cast<std::size_t>(n) + 1, '\0');
        std::snprintf(buf.data(), buf.size(), fmt, detail::ToCArg(items)...);
        buf.resize(static_cast<std::size_t>(n));
        return buf;
      },
      storage);
}

template <typename... Args>
inline std::string Format(std::format_string<Args...> fmt, Args&&... args) {
  return std::format(fmt, std::forward<Args>(args)...);
}

inline std::string FormatSize(uint64_t bytes) {
  if (bytes >= 1024ULL * 1024 * 1024) {
    return std::format("{:.1f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
  }
  if (bytes >= 1024ULL * 1024) {
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024));
  }
  if (bytes >= 1024) {
    return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
  }
  return std::format("{} B", bytes);
}

constexpr bool IsAsciiWhitespace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline std::string_view TrimAsciiWhitespaceView(std::string_view text) noexcept {
  const auto is_ws = [](char c) noexcept { return std::isspace(static_cast<unsigned char>(c)); };
  const auto not_ws = [&](char c) noexcept { return !is_ws(c); };
  const auto start = std::ranges::find_if(text, not_ws);
  if (start == text.end()) {
    return {};
  }
  const auto rstart = std::ranges::find_if(text | std::views::reverse, not_ws);
  const auto begin_idx = static_cast<size_t>(start - text.begin());
  const auto end_idx = static_cast<size_t>(text.rend() - rstart);
  return text.substr(begin_idx, end_idx - begin_idx);
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

inline std::optional<std::pair<std::string_view, std::string_view>>
SplitPairRight(std::string_view text, char delimiter, bool trim_whitespace = false) noexcept {
  const size_t pos = text.rfind(delimiter);
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
  return std::ranges::equal(
      a, b, [](char ca, char cb) noexcept { return ToLowerAsciiChar(ca) == ToLowerAsciiChar(cb); });
}

[[nodiscard]] inline bool StartsWithIgnoreCase(std::string_view text,
                                               std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
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

template <typename Callback>
inline void DrainLinesFromBuffer(std::string& buffer, Callback on_line, bool flush_tail = false) {
  size_t processed = 0;
  size_t start = 0;
  bool should_stop = false;

  while (start < buffer.size()) {
    const size_t end = buffer.find('\n', start);
    if (end == std::string::npos) {
      break;
    }
    size_t len = end - start;
    if (len > 0 && buffer[start + len - 1] == '\r') {
      --len;
    }
    const std::string_view line(buffer.data() + start, len);
    start = end + 1;
    processed = start;

    if constexpr (std::is_void_v<std::invoke_result_t<Callback, std::string_view>>) {
      on_line(line);
    } else {
      if (!static_cast<bool>(on_line(line))) {
        should_stop = true;
        break;
      }
    }
  }

  if (processed > 0) {
    buffer.erase(0, processed);
  }

  if (flush_tail && !should_stop && !buffer.empty()) {
    std::string_view tail(buffer);
    if (!tail.empty() && tail.back() == '\r') {
      tail.remove_suffix(1);
    }
    if constexpr (std::is_void_v<std::invoke_result_t<Callback, std::string_view>>) {
      on_line(tail);
    } else {
      (void)on_line(tail);
    }
    buffer.clear();
  }
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
  if (data.empty()) {
    return {};
  }
  return ::base64::encode_into<std::string>(data.begin(), data.end());
}

inline std::string Base64Encode(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  return ::base64::to_base64(text);
}

inline std::string Base64Encode(const char* data, size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  return ::base64::to_base64(std::string_view(data, size));
}

inline std::string Base64Encode(const unsigned char* data, size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  return ::base64::encode_into<std::string>(data, data + size);
}

inline std::string Base64Decode(std::string_view text, bool remove_linebreaks = false) {
  if (text.empty()) {
    return {};
  }
  if (!remove_linebreaks) {
    return ::base64::from_base64(text);
  }
  std::string sanitized;
  sanitized.reserve(text.size());
  for (char c : text) {
    if (c != '\r' && c != '\n') {
      sanitized.push_back(c);
    }
  }
  return ::base64::from_base64(sanitized);
}

inline bool IsCjkCodepoint(uint32_t cp) noexcept {
  const auto c = static_cast<char32_t>(cp);
  return unicode::is_unified_ideograph(c) || unicode::is_ideographic(c);
}

inline uint32_t FirstUtf8Codepoint(std::string_view s) noexcept {
  if (s.empty()) {
    return 0;
  }
  char32_t cp = 0;
  const size_t len = unicode::utf8::decode_codepoint(s.data(), s.size(), cp);
  return len > 0 ? static_cast<uint32_t>(cp) : 0;
}

inline uint32_t LastUtf8Codepoint(std::string_view s) noexcept {
  if (s.empty()) {
    return 0;
  }
  size_t i = s.size();
  while (i > 0) {
    --i;
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
      char32_t cp = 0;
      const size_t len = unicode::utf8::decode_codepoint(s.data() + i, s.size() - i, cp);
      if (len > 0) {
        return static_cast<uint32_t>(cp);
      }
      break;
    }
  }
  return 0;
}

inline bool IsSentenceEndingPunctuation(uint32_t cp) noexcept {
  const auto c = static_cast<char32_t>(cp);
  return unicode::is_sentence_terminal(c) || c == U'…' || c == U'\n';
}

inline size_t Utf8CodepointWidth(uint32_t codepoint) noexcept {
  const int w =
      unicode::codepoint_width(static_cast<char32_t>(codepoint), unicode::AmbiguousWidth::Wide);
  return w > 0 ? static_cast<size_t>(w) : 0;
}

inline size_t Utf8VisualWidth(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  std::u32string u32;
  unicode::utf8::decode(text.data(), text.size(), u32);
  const int w = unicode::width(u32, unicode::AmbiguousWidth::Wide);
  return w > 0 ? static_cast<size_t>(w) : 0;
}

inline std::string TruncateMiddleUtf8(std::string_view text, size_t max_width,
                                      std::string_view ellipsis = "...") {
  if (max_width == 0) {
    return std::string(text);
  }

  std::u32string u32;
  unicode::utf8::decode(text.data(), text.size(), u32);
  const size_t total_width =
      static_cast<size_t>(std::max(0, unicode::width(u32, unicode::AmbiguousWidth::Wide)));
  if (total_width <= max_width) {
    return std::string(text);
  }

  std::u32string ellipsis_u32;
  unicode::utf8::decode(ellipsis.data(), ellipsis.size(), ellipsis_u32);
  const size_t ellipsis_width =
      static_cast<size_t>(std::max(0, unicode::width(ellipsis_u32, unicode::AmbiguousWidth::Wide)));

  struct GraphemeInfo {
    size_t start;
    size_t length;
    size_t width;
  };
  std::vector<GraphemeInfo> graphemes;
  size_t i = 0;
  while (i < u32.size()) {
    const size_t glen = unicode::grapheme_length(u32.data() + i, u32.size() - i);
    const size_t gwidth = static_cast<size_t>(
        std::max(0, unicode::width(u32.data() + i, glen, unicode::AmbiguousWidth::Wide)));
    graphemes.push_back({i, glen, gwidth});
    i += glen;
  }

  if (max_width <= ellipsis_width + 4) {
    size_t cur_width = 0;
    size_t tail_start_idx = graphemes.size();
    for (size_t g = graphemes.size(); g > 0; --g) {
      const auto& gi = graphemes[g - 1];
      if (cur_width + gi.width > max_width) {
        break;
      }
      cur_width += gi.width;
      tail_start_idx = g - 1;
    }
    if (tail_start_idx >= graphemes.size()) {
      return {};
    }
    const std::u32string tail_u32 = u32.substr(graphemes[tail_start_idx].start);
    std::string result;
    unicode::utf8::encode(tail_u32.data(), tail_u32.size(), result);
    return result;
  }

  const size_t available_content_width = max_width - ellipsis_width;
  size_t target_head_width = std::max<size_t>(6, std::min<size_t>(14, available_content_width / 4));
  if (target_head_width >= available_content_width) {
    target_head_width = available_content_width / 2;
  }
  const size_t target_tail_width = available_content_width - target_head_width;

  size_t head_count = 0;
  size_t cur_head_width = 0;
  for (const auto& gi : graphemes) {
    if (cur_head_width + gi.width > target_head_width) {
      break;
    }
    cur_head_width += gi.width;
    ++head_count;
  }

  size_t tail_start_idx = graphemes.size();
  size_t cur_tail_width = 0;
  for (size_t g = graphemes.size(); g > head_count; --g) {
    const auto& gi = graphemes[g - 1];
    if (cur_tail_width + gi.width > target_tail_width) {
      break;
    }
    cur_tail_width += gi.width;
    tail_start_idx = g - 1;
  }

  const std::u32string head_u32 =
      (head_count > 0)
          ? u32.substr(0, graphemes[head_count - 1].start + graphemes[head_count - 1].length)
          : std::u32string{};
  const std::u32string tail_u32 = (tail_start_idx < graphemes.size())
                                      ? u32.substr(graphemes[tail_start_idx].start)
                                      : std::u32string{};

  std::string head_str;
  unicode::utf8::encode(head_u32.data(), head_u32.size(), head_str);
  std::string tail_str;
  unicode::utf8::encode(tail_u32.data(), tail_u32.size(), tail_str);

  std::string result;
  result.reserve(head_str.size() + ellipsis.size() + tail_str.size());
  result.append(head_str);
  result.append(ellipsis);
  result.append(tail_str);
  return result;
}

inline std::string_view TruncateUtf8Bytes(std::string_view text, size_t max_bytes) noexcept {
  if (text.empty() || max_bytes == 0) {
    return {};
  }
  size_t offset = 0;
  while (offset < text.size()) {
    char32_t cp = 0;
    const size_t len =
        unicode::utf8::decode_codepoint(text.data() + offset, text.size() - offset, cp);
    if (len == 0 || !unicode::is_scalar_value(cp) || offset + len > max_bytes) {
      break;
    }
    offset += len;
  }
  return text.substr(0, offset);
}

inline bool ParseFloatStrict(std::string_view value, float* out = nullptr) noexcept {
  if (value.empty()) {
    return false;
  }
  std::string_view sv = value;
  bool is_negative = false;
  if (sv.starts_with('+')) {
    sv.remove_prefix(1);
    if (sv.empty() || sv.starts_with('+') || sv.starts_with('-')) {
      return false;
    }
  } else if (sv.starts_with('-')) {
    is_negative = true;
    sv.remove_prefix(1);
    if (sv.empty() || sv.starts_with('+') || sv.starts_with('-')) {
      return false;
    }
  }
  std::chars_format fmt = std::chars_format::general;
  if (sv.starts_with("0x") || sv.starts_with("0X")) {
    sv.remove_prefix(2);
    fmt = std::chars_format::hex;
  }
  float val = 0.0F;
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val, fmt);
  if (ec != std::errc{} || ptr != sv.data() + sv.size() || !std::isfinite(val)) {
    return false;
  }
  if (is_negative) {
    val = -val;
  }
  if (out != nullptr) {
    *out = val;
  }
  return true;
}

struct Utf8ValidationResult {
  bool valid = true;
  size_t error_offset = 0;
  const char* error_reason = nullptr;
};

inline Utf8ValidationResult ValidateUtf8Text(std::string_view text, bool allow_control_chars = true,
                                             bool allow_embedded_nul = true) noexcept {
  size_t offset = 0;
  while (offset < text.size()) {
    const auto first = static_cast<unsigned char>(text[offset]);
    size_t length = 0;
    uint32_t codepoint = 0;
    if (first <= 0x7f) {
      length = 1;
      codepoint = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
      codepoint = first & 0x07;
    } else {
      return {false, offset, "invalid UTF-8 leading byte"};
    }

    if (length > text.size() - offset) {
      return {false, offset, "truncated UTF-8 sequence"};
    }
    for (size_t i = 1; i < length; ++i) {
      const auto continuation = static_cast<unsigned char>(text[offset + i]);
      if ((continuation & 0xc0) != 0x80) {
        return {false, offset + i, "invalid UTF-8 continuation byte"};
      }
      codepoint = (codepoint << 6) | (continuation & 0x3f);
    }

    const bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                          (length == 4 && codepoint < 0x10000);
    if (overlong) {
      return {false, offset, "overlong UTF-8 sequence"};
    }
    if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
      return {false, offset, "UTF-8 encodes a surrogate code point"};
    }
    if (codepoint > 0x10ffff) {
      return {false, offset, "UTF-8 code point exceeds U+10FFFF"};
    }
    if (!allow_embedded_nul && codepoint == 0) {
      return {false, offset, "embedded NUL is not allowed"};
    }
    if (!allow_control_chars) {
      const bool permitted_ascii_whitespace =
          codepoint == '\t' || codepoint == '\n' || codepoint == '\r';
      if ((!permitted_ascii_whitespace && codepoint < 0x20) ||
          (codepoint >= 0x7f && codepoint <= 0x9f)) {
        return {false, offset, "control character is not allowed"};
      }
    }
    offset += length;
  }
  return {true, 0, nullptr};
}

inline bool IsValidUtf8(std::string_view text) noexcept {
  size_t offset = 0;
  while (offset < text.size()) {
    char32_t cp = 0;
    const size_t len =
        unicode::utf8::decode_codepoint(text.data() + offset, text.size() - offset, cp);
    if (len == 0 || !unicode::is_scalar_value(cp)) {
      return false;
    }
    offset += len;
  }
  return true;
}

} // namespace vinput::str
