#pragma once

#include <cstdint>
#include <glaze/core/opts.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/write.hpp>
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
  return text == nullptr ? std::move(fallback) : *text;
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
  return number == nullptr ? fallback : static_cast<std::int64_t>(*number);
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
  return glz::json_t(std::move(array));
}

// Builds a JSON object out of a string map.
inline glz::json_t FromStringMap(const std::map<std::string, std::string>& values) {
  glz::json_t::object_t object;
  for (const auto& [key, value] : values) {
    object.emplace(key, glz::json_t(value));
  }
  return glz::json_t(std::move(object));
}

// Shared empty object, used as the fallback of Object().
inline const glz::json_t& EmptyObject() {
  static const glz::json_t value = glz::json_t(glz::json_t::object_t{});
  return value;
}

// Returns the object stored under `key`, or `fallback` when the key is missing
// or holds a non-object value.
inline const glz::json_t& Object(const glz::json_t& object, std::string_view key,
                                 const glz::json_t& fallback = EmptyObject()) {
  const auto* node = Find(object, key);
  if (node == nullptr || !node->is_object()) {
    return fallback;
  }
  return *node;
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
      glz::opts{.skip_null_members = false, .prettify = true, .indentation_width = 2};
  std::string out;
  if (bool(glz::write<kPrettyOpts>(value, out))) {
    return {};
  }
  return out;
}

} // namespace vinput::json
