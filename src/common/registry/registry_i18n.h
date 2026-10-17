#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct CoreConfig;

namespace vinput::registry {

using I18nMap = std::unordered_map<std::string, std::string>;

I18nMap ParseI18nJson(const std::string& content, std::string* error = nullptr);
void ApplyLocalI18nOverrides(I18nMap* map);
std::string DetectPreferredLocale();
I18nMap FetchI18nMap(const std::string& locale, const std::vector<std::string>& urls,
                     std::string* error);
I18nMap FetchMergedI18nMap(const CoreConfig& config, const std::string& preferred_locale,
                           std::string* error = nullptr);
I18nMap LoadMergedCachedI18nMap(const std::string& preferred_locale, std::string* error = nullptr);
std::string LookupI18n(const I18nMap& map, const std::string& key,
                       const std::string& fallback = "");

} // namespace vinput::registry
