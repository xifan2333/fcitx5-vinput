#include "common/registry/registry_i18n.h"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

#include "common/utils/path_utils.h"

namespace vinput::registry {

namespace {

using json = nlohmann::json;

std::string NormalizeLocale(std::string locale) {
  if (locale.empty()) {
    return {};
  }
  const auto colon_pos = locale.find(':');
  if (colon_pos != std::string::npos) {
    locale = locale.substr(0, colon_pos);
  }
  const auto dot_pos = locale.find('.');
  if (dot_pos != std::string::npos) {
    locale = locale.substr(0, dot_pos);
  }
  const auto at_pos = locale.find('@');
  if (at_pos != std::string::npos) {
    locale = locale.substr(0, at_pos);
  }
  for (char& ch : locale) {
    if (ch == '-') {
      ch = '_';
    }
  }
  if (locale == "C" || locale == "POSIX") {
    return {};
  }
  const auto sep = locale.find('_');
  if (sep == std::string::npos) {
    if (locale == "zh") {
      return "zh_CN";
    }
    if (locale == "en") {
      return "en_US";
    }
  }
  return locale;
}

} // namespace

I18nMap ParseI18nJson(const std::string& content, std::string* error) {
  I18nMap map;

  try {
    json j = json::parse(content);
    if (!j.is_object()) {
      if (error) {
        *error = "i18n JSON is not an object";
      }
      return map;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.value().is_string()) {
        map[it.key()] = it.value().get<std::string>();
      }
    }
  } catch (const std::exception& ex) {
    if (error) {
      *error = std::string("failed to parse i18n JSON: ") + ex.what();
    }
    return {};
  }

  return map;
}

namespace {

I18nMap LoadI18nFile(const std::filesystem::path& path, std::string* error) {
  std::ifstream ifs(path);
  if (!ifs) {
    if (error) {
      *error = "cached i18n unavailable: " + path.string();
    }
    return {};
  }

  std::string content(std::istreambuf_iterator<char>(ifs), {});
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
  const char* vars[] = {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
  for (const char* name : vars) {
    const char* value = std::getenv(name);
    if (value && *value) {
      const std::string locale = NormalizeLocale(value);
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
  if (error) {
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
