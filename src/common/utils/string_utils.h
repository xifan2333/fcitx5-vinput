#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
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

// =========================================================================
// 1. Formatting (i18n Gettext Bridge)
// =========================================================================

template <typename... Args> std::string FmtStr(const char* fmt, const Args&... args) {
  if (fmt == nullptr) {
    return {};
  }
  if constexpr (sizeof...(Args) == 0) {
    // No substitution requested (common for plain gettext messages); returning the
    // text verbatim also avoids passing a non-literal format to snprintf
    // (-Wformat-security).
    return {fmt};
  } else {
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
}

// =========================================================================
// 2. Trimming (Zero-Copy)
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

} // namespace vinput::str
