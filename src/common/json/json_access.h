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
  glz::json_t::array_t array;
  array.reserve(values.size());
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return {std::move(array)};
}

// Builds a JSON object out of a string map.
inline glz::json_t FromStringMap(const std::map<std::string, std::string>& values) {
  glz::json_t::object_t object;
  for (const auto& [key, value] : values) {
    object.emplace(key, glz::json_t(value));
  }
  return {std::move(object)};
}

// Shared empty object, used as the fallback of Object().
inline const glz::json_t& EmptyObject() {
  static const glz::json_t value = glz::json_t(glz::json_t::object_t{});
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
inline bool IsSingleJsonValue(std::string_view text) {
  std::size_t pos = 0;
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  if (pos >= text.size()) {
    return false;
  }

  const char first = text[pos];
  if (first == '{' || first == '[') {
    int depth = 0;
    bool in_string = false;
    for (; pos < text.size(); ++pos) {
      const char c = text[pos];
      if (in_string) {
        if (c == '\\') {
          ++pos;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
      } else if (c == '{' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ']') {
        --depth;
        if (depth == 0) {
          ++pos;
          break;
        }
        if (depth < 0) {
          return false;
        }
      }
    }
    if (depth != 0 || in_string) {
      return false;
    }
  } else if (first == '"') {
    for (++pos; pos < text.size(); ++pos) {
      if (text[pos] == '\\') {
        ++pos;
      } else if (text[pos] == '"') {
        ++pos;
        break;
      }
    }
  } else {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) == 0) {
      ++pos;
    }
  }

  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  return pos == text.size();
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
