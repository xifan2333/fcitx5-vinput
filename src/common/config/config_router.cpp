#include "common/config/config_router.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <glaze/core/opts.hpp>
#include <glaze/core/read.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace vinput::config {

namespace {

// User-edited trees may carry unknown keys, which must not abort the read.
constexpr auto kTreeReadOpts = glz::opts{.error_on_unknown_keys = uint8_t{0}};

bool LoadConfigTree(glz::json_t* root) {
  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);
  const std::string text = CoreConfigToJson(config);
  return !bool(glz::read<kTreeReadOpts>(*root, text));
}

glz::json_t* ChildAt(glz::json_t* node, const std::string& segment) {
  if (auto* object = node->get_if<glz::json_t::object_t>()) {
    const auto it = object->find(segment);
    return it == object->end() ? nullptr : &it->second;
  }
  auto* array = node->get_if<glz::json_t::array_t>();
  if (array == nullptr) {
    return nullptr;
  }
  std::size_t index = 0;
  const char* begin = segment.data();
  const char* end = begin + segment.size();
  const auto [parse_end, parse_error] = std::from_chars(begin, end, index);
  if (parse_error != std::errc{} || parse_end != end || index >= array->size()) {
    return nullptr;
  }
  return &(*array)[index];
}

// Resolves an RFC 6901 JSON pointer against a dynamic JSON tree. Returns nullptr
// when any segment is missing.
glz::json_t* SeekPointer(glz::json_t* root, std::string_view pointer) {
  glz::json_t* node = root;
  std::size_t pos = 0;
  while (pos < pointer.size()) {
    if (pointer[pos] != '/') {
      return nullptr;
    }
    ++pos;

    std::string segment;
    while (pos < pointer.size() && pointer[pos] != '/') {
      const char current = pointer[pos];
      ++pos;
      if (current != '~') {
        segment.push_back(current);
        continue;
      }
      // RFC 6901 escapes: `~1` is '/', `~0` is '~'. Anything else is invalid.
      if (pos >= pointer.size()) {
        return nullptr;
      }
      const char escaped = pointer[pos];
      ++pos;
      if (escaped == '1') {
        segment.push_back('/');
      } else if (escaped == '0') {
        segment.push_back('~');
      } else {
        return nullptr;
      }
    }

    node = ChildAt(node, segment);
    if (node == nullptr) {
      return nullptr;
    }
  }
  return node;
}

std::string FormatNode(const glz::json_t& node) {
  std::string out;
  if (bool(glz::write_json(node, out))) {
    return {};
  }
  return out;
}

void ParseSetValue(const std::string& value, glz::json_t* parsed) {
  if (vinput::json::IsSingleJsonValue(value) && !bool(glz::read_json(*parsed, value))) {
    return;
  }
  // Not a single standalone JSON value: treat the argument as a bare string.
  *parsed = glz::json_t(value);
}

} // namespace

bool GetConfigValue(const std::string& pointer, std::string* value, std::string* error) {
  glz::json_t root;
  if (!LoadConfigTree(&root)) {
    if (error != nullptr) {
      *error = "Failed to load config.";
    }
    return false;
  }

  const glz::json_t* node = SeekPointer(&root, pointer);
  if (node == nullptr) {
    if (error != nullptr) {
      *error = "Config path not found: " + pointer;
    }
    return false;
  }

  if (value != nullptr) {
    if (const auto* text = node->get_if<std::string>()) {
      *value = *text;
    } else {
      *value = FormatNode(*node);
    }
  }
  return true;
}

bool SetConfigValue(const std::string& pointer, const std::string& value, std::string* error) {
  glz::json_t root;
  if (!LoadConfigTree(&root)) {
    if (error != nullptr) {
      *error = "Failed to load config.";
    }
    return false;
  }

  glz::json_t* node = SeekPointer(&root, pointer);
  if (node == nullptr) {
    if (error != nullptr) {
      *error = "Config path not found: " + pointer;
    }
    return false;
  }

  glz::json_t parsed;
  ParseSetValue(value, &parsed);
  *node = std::move(parsed);

  CoreConfig updated;
  if (!CoreConfigFromJson(FormatNode(root), &updated, error)) {
    return false;
  }

  if (!SaveCoreConfig(updated)) {
    if (error != nullptr) {
      std::string validation_error;
      NormalizeCoreConfig(&updated);
      if (ValidateCoreConfig(updated, &validation_error) && validation_error.empty()) {
        *error = "Failed to save config.";
      } else {
        *error = validation_error;
      }
    }
    return false;
  }
  return true;
}

std::filesystem::path GetEditTarget(const std::string& target) {
  std::filesystem::path path;
  if (target == "core") {
    path = vinput::path::CoreConfigPath();
  } else {
    path = vinput::path::FcitxAddonConfigPath();
  }

  std::filesystem::path resolved;
  if (vinput::file::ResolveSymlinkPath(path, &resolved, nullptr)) {
    return resolved;
  }
  return path;
}

} // namespace vinput::config
