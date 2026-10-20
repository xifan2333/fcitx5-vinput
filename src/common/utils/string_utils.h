#pragma once
#include <algorithm>
#include <base64.h>
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
#include <uni_algo/conv.h>
#include <uni_algo/ranges_conv.h>
#include <uni_algo/ranges_grapheme.h>
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
  return ::base64_encode(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

inline std::string Base64Encode(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  return ::base64_encode(text);
}

inline std::string Base64Encode(const char* data, size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  return ::base64_encode(reinterpret_cast<const unsigned char*>(data), size);
}

inline std::string Base64Encode(const unsigned char* data, size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  return ::base64_encode(data, size);
}

inline std::string Base64Decode(std::string_view text, bool remove_linebreaks = false) {
  if (text.empty()) {
    return {};
  }
  return ::base64_decode(text, remove_linebreaks);
}

inline bool IsCjkCodepoint(uint32_t cp) noexcept {
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x2FA1F) || (cp >= 0x30000 && cp <= 0x3134F);
}

inline uint32_t FirstUtf8Codepoint(std::string_view s) {
  if (s.empty()) {
    return 0;
  }
  auto view = una::views::utf8(s);
  auto it = view.begin();
  if (it != view.end()) {
    return static_cast<uint32_t>(*it);
  }
  return 0;
}

inline uint32_t LastUtf8Codepoint(std::string_view s) {
  if (s.empty()) {
    return 0;
  }
  auto view = una::views::utf8(s);
  auto it = view.end();
  if (it != view.begin()) {
    --it;
    return static_cast<uint32_t>(*it);
  }
  return 0;
}

inline bool IsSentenceEndingPunctuation(uint32_t cp) noexcept {
  // CJK sentence enders: 。！？…
  if (cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == 0x2026) {
    return true;
  }
  // Latin sentence enders
  if (cp == '.' || cp == '!' || cp == '?' || cp == '\n') {
    return true;
  }
  return false;
}

inline size_t Utf8CodepointWidth(uint32_t codepoint) noexcept {
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

inline size_t Utf8VisualWidth(std::string_view text) {
  size_t width = 0;
  for (char32_t cp : una::views::utf8(text)) {
    width += Utf8CodepointWidth(static_cast<uint32_t>(cp));
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
    size_t current_width = 0;
    auto view = una::views::grapheme::utf8(text);
    auto it = view.end();
    const char* tail_start = text.data() + text.size();
    while (it != view.begin()) {
      --it;
      const std::string_view g = *it;
      const size_t cp_width = Utf8CodepointWidth(FirstUtf8Codepoint(g));
      if (current_width + cp_width > tail_budget) {
        break;
      }
      current_width += cp_width;
      tail_start = it.begin();
    }
    return std::string(tail_start, (text.data() + text.size()) - tail_start);
  }

  const size_t available_content_width = max_width - ellipsis_width;
  size_t target_head_width = std::max<size_t>(6, std::min<size_t>(14, available_content_width / 4));
  if (target_head_width >= available_content_width) {
    target_head_width = available_content_width / 2;
  }
  const size_t target_tail_width = available_content_width - target_head_width;

  size_t head_cur_width = 0;
  const char* head_end = text.data();
  auto head_view = una::views::grapheme::utf8(text);
  for (auto it = head_view.begin(); it != head_view.end(); ++it) {
    const std::string_view g = *it;
    const size_t cp_width = Utf8CodepointWidth(FirstUtf8Codepoint(g));
    if (head_cur_width + cp_width > target_head_width) {
      break;
    }
    head_cur_width += cp_width;
    head_end = it.end();
  }

  size_t tail_cur_width = 0;
  const char* tail_start = text.data() + text.size();
  auto tail_view = una::views::grapheme::utf8(text);
  auto tail_it = tail_view.end();
  while (tail_it != tail_view.begin()) {
    --tail_it;
    if (tail_it.begin() < head_end) {
      break;
    }
    const std::string_view g = *tail_it;
    const size_t cp_width = Utf8CodepointWidth(FirstUtf8Codepoint(g));
    if (tail_cur_width + cp_width > target_tail_width) {
      break;
    }
    tail_cur_width += cp_width;
    tail_start = tail_it.begin();
  }

  std::string result;
  const size_t head_bytes = static_cast<size_t>(head_end - text.data());
  const size_t tail_bytes = static_cast<size_t>((text.data() + text.size()) - tail_start);
  result.reserve(head_bytes + ellipsis.size() + tail_bytes);
  result.append(text.substr(0, head_bytes));
  result.append(ellipsis);
  result.append(text.substr(static_cast<size_t>(tail_start - text.data())));
  return result;
}

inline std::string_view TruncateUtf8Bytes(std::string_view text, size_t max_bytes) noexcept {
  if (text.empty() || max_bytes == 0) {
    return {};
  }
  una::error err;
  const bool valid = una::is_valid_utf8(text, err);
  const size_t valid_len = valid ? text.size() : err.pos();
  const std::string_view valid_text = text.substr(0, valid_len);
  if (valid_text.size() <= max_bytes) {
    return valid_text;
  }
  size_t last_valid = 0;
  for (auto it = una::views::utf8(valid_text).begin(); it != una::views::utf8(valid_text).end();
       ++it) {
    const size_t next_len = static_cast<size_t>(it.end() - valid_text.data());
    if (next_len > max_bytes) {
      break;
    }
    last_valid = next_len;
  }
  return valid_text.substr(0, last_valid);
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
  return una::is_valid_utf8(text);
}

} // namespace vinput::str
