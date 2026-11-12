#include "common/registry/registry_cache.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/utils/downloader.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace vinput::registry::cache {

namespace {

bool LoadCachedText(const std::filesystem::path& cache_path, std::string* content,
                    vinput::download::Result* result, std::string* error) {
  auto read_res = vinput::file::ReadTextFile(cache_path);
  if (!read_res) {
    if (error != nullptr) {
      *error = read_res.error();
    }
    return false;
  }
  if (content != nullptr) {
    *content = std::move(*read_res);
  }

  if (result != nullptr) {
    result->ok = true;
    result->resolved_url = cache_path.string();
    result->http_code = 0;
    result->bytes_received = (content != nullptr) ? content->size() : 0;
    result->error.clear();
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool SaveCachedText(const std::filesystem::path& cache_path, const std::string& content,
                    std::string* error) {
  auto res = vinput::file::AtomicWriteTextFile(cache_path, content);
  if (!res) {
    if (error != nullptr) {
      *error = res.error();
    }
    return false;
  }
  return true;
}

std::string FormatFetchFailure(std::string_view download_error, std::string_view cache_error) {
  if (!download_error.empty()) {
    return std::string(download_error) + "; cache unavailable: " + std::string(cache_error);
  }
  return std::string(cache_error);
}

bool HandleDownloadSuccess(const std::filesystem::path& cache_path, std::string* content,
                           vinput::download::Result&& download_result,
                           vinput::download::Result* result, std::string* error) {
  std::string write_error;
  const std::string& cached_content = (content != nullptr) ? *content : std::string{};
  const bool saved = SaveCachedText(cache_path, cached_content, &write_error);
  if (result != nullptr) {
    *result = std::move(download_result);
  }
  if (!saved) {
    if (error != nullptr) {
      *error = write_error;
    }
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool TryLoadCachedFallback(const std::filesystem::path& cache_path, std::string_view download_error,
                           std::string* content, vinput::download::Result* result,
                           std::string* error, FetchStatus* status) {
  std::string cache_error;
  if (LoadCachedText(cache_path, content, result, &cache_error)) {
    if (status != nullptr) {
      status->used_cache = true;
      status->fallback_error = std::string(download_error);
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  if (error != nullptr) {
    *error = FormatFetchFailure(download_error, cache_error);
  }
  return false;
}

std::filesystem::path CachePath(std::string_view name) {
  return vinput::path::RegistryCacheDir() / name;
}

} // namespace

bool FetchText(const std::vector<std::string>& urls, const std::filesystem::path& cache_path,
               const vinput::download::Options& options, std::string* content,
               vinput::download::Result* result, std::string* error, FetchStatus* status) {
  if (content != nullptr) {
    content->clear();
  }
  if (status != nullptr) {
    *status = {};
  }

  vinput::download::Result download_result;
  if (vinput::download::DownloadText(urls, options, content, &download_result)) {
    return HandleDownloadSuccess(cache_path, content, std::move(download_result), result, error);
  }

  if (TryLoadCachedFallback(cache_path, download_result.error, content, result, error, status)) {
    return true;
  }

  if (result != nullptr) {
    *result = std::move(download_result);
  }
  return false;
}

std::filesystem::path ModelRegistryPath() {
  return CachePath("models.json");
}

std::filesystem::path AsrProviderRegistryPath() {
  return CachePath("providers.json");
}

std::filesystem::path LlmAdapterRegistryPath() {
  return CachePath("adapters.json");
}

std::filesystem::path I18nPath(const std::string& locale) {
  return vinput::path::I18nCachePath(locale);
}

} // namespace vinput::registry::cache
