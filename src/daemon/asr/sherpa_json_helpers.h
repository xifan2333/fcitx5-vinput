#pragma once

#include <cmath>
#include <glaze/json/json_t.hpp>
#include <limits>
#include <string>
#include <string_view>

#include "common/json/json_access.h"

namespace vinput::daemon::asr {

inline float SafeStof(const std::string& s, float default_val) {
  try {
    return std::stof(s);
  } catch (...) {
    return default_val;
  }
}

inline int SafeStoi(const std::string& s, int default_val) {
  try {
    return std::stoi(s);
  } catch (...) {
    return default_val;
  }
}

inline std::string JsonString(const glz::json_t& obj, std::string_view key,
                              const std::string& default_val = {}) {
  const auto* node = vinput::json::Find(obj, key);
  if (node == nullptr) {
    return default_val;
  }
  if (const auto* text = node->get_if<std::string>()) {
    return *text;
  }
  return default_val;
}

// Model metadata mixes numbers, booleans and numeric strings for the same knob,
// so every JSON kind that can carry a magnitude is accepted.
inline int JsonInt(const glz::json_t& obj, std::string_view key, int default_val) {
  const auto* node = vinput::json::Find(obj, key);
  if (node == nullptr) {
    return default_val;
  }
  if (const auto* number = node->get_if<double>()) {
    if (!std::isfinite(*number) || std::trunc(*number) != *number ||
        *number < static_cast<double>(std::numeric_limits<int>::min()) ||
        *number > static_cast<double>(std::numeric_limits<int>::max())) {
      return default_val;
    }
    return static_cast<int>(*number);
  }
  if (const auto* flag = node->get_if<bool>()) {
    return *flag ? 1 : 0;
  }
  if (const auto* text = node->get_if<std::string>()) {
    return SafeStoi(*text, default_val);
  }
  return default_val;
}

inline float JsonFloat(const glz::json_t& obj, std::string_view key, float default_val) {
  const auto* node = vinput::json::Find(obj, key);
  if (node == nullptr) {
    return default_val;
  }
  if (const auto* number = node->get_if<double>()) {
    return static_cast<float>(*number);
  }
  if (const auto* text = node->get_if<std::string>()) {
    return SafeStof(*text, default_val);
  }
  return default_val;
}

inline bool JsonBool(const glz::json_t& obj, std::string_view key, bool default_val = false) {
  const auto* node = vinput::json::Find(obj, key);
  if (node == nullptr) {
    return default_val;
  }
  if (const auto* flag = node->get_if<bool>()) {
    return *flag;
  }
  if (const auto* number = node->get_if<double>()) {
    return *number != 0.0;
  }
  if (const auto* text = node->get_if<std::string>()) {
    return *text == "true" || *text == "1";
  }
  return default_val;
}

} // namespace vinput::daemon::asr
