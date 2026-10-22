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
    std::string write_error;
    if (!SaveCachedText(cache_path, (content != nullptr) ? *content : std::string{},
                        &write_error)) {
      if (error != nullptr) {
        *error = write_error;
      }
      if (result != nullptr) {
        *result = std::move(download_result);
      }
      return false;
    }
    if (result != nullptr) {
      *result = std::move(download_result);
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  std::string cache_error;
  if (LoadCachedText(cache_path, content, result, &cache_error)) {
    if (status != nullptr) {
      status->used_cache = true;
      status->fallback_error = download_result.error;
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  if (error != nullptr) {
    if (!download_result.error.empty()) {
      *error = download_result.error + "; cache unavailable: " + cache_error;
    } else {
      *error = cache_error;
    }
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
