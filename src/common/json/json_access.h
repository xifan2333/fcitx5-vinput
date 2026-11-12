#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glaze/core/opts.hpp>
#include <glaze/core/write.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/write.hpp>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vinput::json {

// Typed accessors for dynamically shaped JSON trees. Glaze's compile time
// reflection over concrete structs is preferred whenever the shape is known;
// these helpers exist only for trees whose keys are discovered at runtime
// (registries, model manifests, provider responses, user edited patches).

inline const glz::json_t* Find(const glz::json_t& object, std::string_view key) {
  const auto* members = object.get_if<glz::json_t::object_t>();
  if (members == nullptr) {
    return nullptr;
  }
  const auto it = members->find(key);
  return it == members->end() ? nullptr : &it->second;
}

inline glz::json_t* Find(glz::json_t& object, std::string_view key) {
  auto* members = object.get_if<glz::json_t::object_t>();
  if (members == nullptr) {
    return nullptr;
  }
  const auto it = members->find(key);
  return it == members->end() ? nullptr : &it->second;
}

inline std::string String(const glz::json_t& object, std::string_view key,
                          std::string fallback = {}) {
  const auto* node = Find(object, key);
  if (node == nullptr) {
    return fallback;
  }
  const auto* text = node->get_if<std::string>();
  return text == nullptr ? fallback : *text;
}

inline double Number(const glz::json_t& object, std::string_view key, double fallback = 0.0) {
  const auto* node = Find(object, key);
  if (node == nullptr) {
    return fallback;
  }
  const auto* number = node->get_if<double>();
  return number == nullptr ? fallback : *number;
}

inline std::int64_t Int64(const glz::json_t& object, std::string_view key,
                          std::int64_t fallback = 0) {
  const auto* node = Find(object, key);
  if (node == nullptr) {
    return fallback;
  }
  const auto* number = node->get_if<double>();
  if (number == nullptr) {
    return fallback;
  }
  // External documents can carry any magnitude, and converting a
  // non-representable double to an integer is undefined behaviour.
  if (!std::isfinite(*number) || std::trunc(*number) != *number ||
      *number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      *number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return fallback;
  }
  return static_cast<std::int64_t>(*number);
}

inline int Int(const glz::json_t& object, std::string_view key, int fallback = 0) {
  return static_cast<int>(Int64(object, key, fallback));
}

inline bool Bool(const glz::json_t& object, std::string_view key, bool fallback = false) {
  const auto* node = Find(object, key);
  if (node == nullptr) {
    return fallback;
  }
  const auto* flag = node->get_if<bool>();
  return flag == nullptr ? fallback : *flag;
}

// Reads every string element of `value`, ignoring other element kinds.
// Non-array values yield an empty vector.
inline std::vector<std::string> Strings(const glz::json_t& value) {
  const auto* array = value.get_if<glz::json_t::array_t>();
  if (array == nullptr) {
    return {};
  }
  std::vector<std::string> values;
  values.reserve(array->size());
  for (const auto& item : *array) {
    if (const auto* text = item.get_if<std::string>()) {
      values.push_back(*text);
    }
  }
  return values;
}

// Reads every string element of the array stored under `key`.
inline std::vector<std::string> Strings(const glz::json_t& object, std::string_view key) {
  const auto* node = Find(object, key);
  return node == nullptr ? std::vector<std::string>{} : Strings(*node);
}

// Reads every string valued member of the object stored under `key`.
inline std::map<std::string, std::string> StringMap(const glz::json_t& value) {
  const auto* members = value.get_if<glz::json_t::object_t>();
  if (members == nullptr) {
    return {};
  }
  std::map<std::string, std::string> values;
  for (const auto& [member_key, member_value] : *members) {
    if (const auto* text = member_value.get_if<std::string>()) {
      values.emplace(member_key, *text);
    }
  }
  return values;
}

inline std::map<std::string, std::string> StringMap(const glz::json_t& object,
                                                    std::string_view key) {
  const auto* node = Find(object, key);
  return node == nullptr ? std::map<std::string, std::string>{} : StringMap(*node);
}

// Builds a JSON array out of a string container.
template <typename Container> glz::json_t FromStrings(const Container& values) {
  glz::json_t result;
  auto& array = result.data.emplace<glz::json_t::array_t>();
  array.reserve(values.size());
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return result;
}

// Builds a JSON object out of a string map.
inline glz::json_t FromStringMap(const std::map<std::string, std::string>& values) {
  glz::json_t result;
  auto& object = result.data.emplace<glz::json_t::object_t>();
  for (const auto& [key, value] : values) {
    object.emplace(key, value);
  }
  return result;
}

// Shared empty object, used as the fallback of Object().
inline const glz::json_t& EmptyObject() {
  static const glz::json_t value = [] {
    glz::json_t out;
    out.data.emplace<glz::json_t::object_t>();
    return out;
  }();
  return value;
}

// Returns the object stored under `key`, or the shared empty object when the key
// is missing or holds a non-object value.
inline const glz::json_t& Object(const glz::json_t& object, std::string_view key) {
  const auto* node = Find(object, key);
  if (node == nullptr || !node->is_object()) {
    return EmptyObject();
  }
  return *node;
}

// Glaze's reader stops after the first complete JSON value, so a document such
// as `{"a":1} junk` would otherwise parse successfully. User supplied JSON is
// validated to hold exactly one value before it reaches the reader.
namespace detail {

inline std::size_t SkipWhitespace(std::string_view text, std::size_t pos) noexcept {
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  return pos;
}

inline std::optional<std::size_t> ScanQuotedString(std::string_view text,
                                                   std::size_t pos) noexcept {
  if (pos >= text.size() || text[pos] != '"') {
    return std::nullopt;
  }
  for (++pos; pos < text.size(); ++pos) {
    if (text[pos] == '\\') {
      ++pos;
    } else if (text[pos] == '"') {
      return pos + 1;
    }
  }
  return std::nullopt;
}

inline int DelimiterDelta(char c) noexcept {
  if (c == '{' || c == '[') {
    return 1;
  }
  if (c == '}' || c == ']') {
    return -1;
  }
  return 0;
}

inline std::optional<std::size_t> ScanCompoundValue(std::string_view text,
                                                    std::size_t pos) noexcept {
  int depth = 0;
  while (pos < text.size()) {
    const char c = text[pos];
    if (c == '"') {
      const auto next_pos = ScanQuotedString(text, pos);
      if (!next_pos.has_value()) {
        return std::nullopt;
      }
      pos = *next_pos;
      continue;
    }
    depth += DelimiterDelta(c);
    if (depth == 0) {
      return pos + 1;
    }
    if (depth < 0) {
      return std::nullopt;
    }
    ++pos;
  }
  return std::nullopt;
}

inline std::size_t ScanScalarValue(std::string_view text, std::size_t pos) noexcept {
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) == 0) {
    ++pos;
  }
  return pos;
}

inline std::optional<std::size_t> ScanSingleValue(std::string_view text, std::size_t pos) noexcept {
  if (pos >= text.size()) {
    return std::nullopt;
  }
  const char first = text[pos];
  if (first == '{' || first == '[') {
    return ScanCompoundValue(text, pos);
  }
  if (first == '"') {
    return ScanQuotedString(text, pos);
  }
  return ScanScalarValue(text, pos);
}

} // namespace detail

inline bool IsSingleJsonValue(std::string_view text) {
  const std::size_t start = detail::SkipWhitespace(text, 0);
  const auto end = detail::ScanSingleValue(text, start);
  if (!end.has_value()) {
    return false;
  }
  const std::size_t trailing = detail::SkipWhitespace(text, *end);
  return trailing == text.size();
}

// Compact JSON text of `value`; empty when serialization fails.
inline std::string ToText(const glz::json_t& value) {
  std::string out;
  if (bool(glz::write_json(value, out))) {
    return {};
  }
  return out;
}

// Two space indented JSON text of `value`; empty when serialization fails.
inline std::string ToPrettyText(const glz::json_t& value) {
  constexpr auto kPrettyOpts =
      glz::opts{.skip_null_members = uint8_t{0}, .prettify = uint8_t{1}, .indentation_width = 2};
  std::string out;
  if (bool(glz::write<kPrettyOpts>(value, out))) {
    return {};
  }
  return out;
}

} // namespace vinput::json
