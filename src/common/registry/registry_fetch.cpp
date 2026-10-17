#include "common/registry/registry_fetch.h"

#include "common/config/core_config.h"
#include "common/registry/registry_cache.h"
#include "common/registry/registry_i18n.h"

namespace vinput::registry {

namespace {

std::string CacheFallbackWarning(const std::string& name, const cache::FetchStatus& status) {
  std::string warning = "using cached " + name + " because download failed";
  if (!status.fallback_error.empty()) {
    warning += ": " + status.fallback_error;
  }
  return warning;
}

bool RefreshLocaleCache(const CoreConfig& config, const std::string& locale, std::string* error,
                        std::vector<std::string>* warnings) {
  const auto urls = ResolveRegistryI18nUrls(config, locale);
  if (urls.empty()) {
    if (error) {
      *error = "no i18n URLs configured";
    }
    return false;
  }

  std::string content;
  vinput::download::Options options;
  options.timeout_seconds = 20;
  options.max_bytes = 1024 * 1024;
  cache::FetchStatus status;
  const bool ok = vinput::registry::cache::FetchText(
      urls, vinput::registry::cache::I18nPath(locale), options, &content, nullptr, error, &status);
  if (ok && status.used_cache && warnings) {
    warnings->push_back(CacheFallbackWarning("i18n " + locale, status));
  }
  return ok;
}

void RefreshI18nAfterOnlineFetch(const CoreConfig* config, std::vector<std::string>* warnings) {
  if (!config) {
    return;
  }

  const std::string preferred_locale = DetectPreferredLocale();
  std::string ignored_error;
  RefreshLocaleCache(*config, preferred_locale, &ignored_error, warnings);
  if (preferred_locale != "en_US") {
    RefreshLocaleCache(*config, "en_US", nullptr, warnings);
  }
}

} // namespace

bool FetchRegistryText(const CoreConfig* config, const std::vector<std::string>& urls,
                       const std::filesystem::path& cache_path,
                       const vinput::download::Options& options, std::string* content,
                       vinput::download::Result* result, std::string* error,
                       std::vector<std::string>* warnings) {
  vinput::download::Result local_result;
  cache::FetchStatus status;
  const bool ok = vinput::registry::cache::FetchText(urls, cache_path, options, content,
                                                     &local_result, error, &status);
  if (!ok) {
    if (result) {
      *result = std::move(local_result);
    }
    return false;
  }

  if (status.used_cache) {
    if (warnings) {
      warnings->push_back(CacheFallbackWarning(cache_path.filename().string(), status));
    }
  } else {
    RefreshI18nAfterOnlineFetch(config, warnings);
  }

  if (result) {
    *result = std::move(local_result);
  }
  return true;
}

I18nMap FetchI18nMap(const std::string& locale, const std::vector<std::string>& urls,
                     std::string* error) {
  if (urls.empty()) {
    if (error != nullptr) {
      *error = "no i18n URLs configured";
    }
    return {};
  }
  std::string content;
  vinput::download::Result result;
  vinput::download::Options options;
  options.timeout_seconds = 20;
  options.max_bytes = 1024ULL * 1024ULL;
  if (!vinput::registry::cache::FetchText(urls, vinput::registry::cache::I18nPath(locale), options,
                                          &content, &result, error)) {
    return {};
  }
  return ParseI18nJson(content, error);
}

I18nMap FetchMergedI18nMap(const CoreConfig& config, const std::string& preferred_locale,
                           std::string* error) {
  I18nMap merged;
  std::string fetch_error;

  const auto primary_urls = ResolveRegistryI18nUrls(config, preferred_locale);
  if (!primary_urls.empty()) {
    merged = FetchI18nMap(preferred_locale, primary_urls, &fetch_error);
  }

  if (merged.empty() && preferred_locale != "en_US") {
    const auto fallback_urls = ResolveRegistryI18nUrls(config, "en_US");
    auto fallback = FetchI18nMap("en_US", fallback_urls, &fetch_error);
    merged.insert(fallback.begin(), fallback.end());
  } else if (preferred_locale != "en_US") {
    const auto fallback_urls = ResolveRegistryI18nUrls(config, "en_US");
    auto fallback = FetchI18nMap("en_US", fallback_urls, nullptr);
    for (auto& [key, value] : fallback) {
      merged.emplace(key, std::move(value));
    }
  }

  if (error != nullptr) {
    *error = fetch_error;
  }

  ApplyLocalI18nOverrides(&merged);
  return merged;
}

} // namespace vinput::registry
