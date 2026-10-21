#include "common/registry/registry_i18n.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glaze/core/common.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <iterator>
#include <string>
#include <utility>

#include "common/utils/locale_utils.h"
#include "common/utils/path_utils.h"

namespace vinput::registry {

I18nMap ParseI18nJson(const std::string& content, std::string* error) {
  I18nMap map;

  glz::json_t root;
  if (const auto ec = glz::read_json(root, content); bool(ec)) {
    if (error != nullptr) {
      *error = "failed to parse i18n JSON: " + glz::format_error(ec, content);
    }
    return {};
  }
  const auto* entries = root.get_if<glz::json_t::object_t>();
  if (entries == nullptr) {
    if (error != nullptr) {
      *error = "i18n JSON is not an object";
    }
    return map;
  }
  for (const auto& [key, value] : *entries) {
    if (const auto* text = value.get_if<std::string>()) {
      map[key] = *text;
    }
  }

  return map;
}

namespace {

I18nMap LoadI18nFile(const std::filesystem::path& path, std::string* error) {
  std::ifstream ifs(path);
  if (!ifs) {
    if (error != nullptr) {
      *error = "cached i18n unavailable: " + path.string();
    }
    return {};
  }

  const std::string content(std::istreambuf_iterator<char>(ifs), {});
  return ParseI18nJson(content, error);
}

I18nMap LoadLocalI18nOverrides() {
  std::string ignored_error;
  return LoadI18nFile(vinput::path::VinputConfigDir() / "i18n.local.json", &ignored_error);
}

} // namespace

void ApplyLocalI18nOverrides(I18nMap* map) {
  auto local = LoadLocalI18nOverrides();
  for (auto& [key, value] : local) {
    (*map)[key] = std::move(value);
  }
}

std::string DetectPreferredLocale() {
  const char* const vars[] = {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
  for (const char* name : vars) {
    const char* value = std::getenv(name);
    if (value != nullptr && *value != '\0') {
      const std::string locale = vinput::locale::CanonicalizeLocale(value);
      if (!locale.empty()) {
        return locale;
      }
    }
  }
  return "en_US";
}

I18nMap LoadMergedCachedI18nMap(const std::string& preferred_locale, std::string* error) {
  std::string primary_error;
  I18nMap merged = LoadI18nFile(vinput::path::I18nCachePath(preferred_locale), &primary_error);

  std::string fallback_error;
  if (preferred_locale != "en_US") {
    auto fallback = LoadI18nFile(vinput::path::I18nCachePath("en_US"), &fallback_error);
    if (merged.empty()) {
      merged = std::move(fallback);
    } else {
      for (auto& [key, value] : fallback) {
        merged.emplace(key, std::move(value));
      }
    }
  }

  ApplyLocalI18nOverrides(&merged);
  if (error != nullptr) {
    if (!merged.empty()) {
      error->clear();
    } else if (!primary_error.empty()) {
      *error = primary_error;
      if (!fallback_error.empty()) {
        *error += "; " + fallback_error;
      }
    } else {
      *error = fallback_error;
    }
  }
  return merged;
}

std::string LookupI18n(const I18nMap& map, const std::string& key, const std::string& fallback) {
  const auto it = map.find(key);
  if (it == map.end() || it->second.empty()) {
    return fallback;
  }
  return it->second;
}

} // namespace vinput::registry
