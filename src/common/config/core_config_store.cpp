#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace {

struct ConfigCache {
  std::mutex mu;
  bool hasCache = false;
  std::filesystem::file_time_type mtime;
  std::uintmax_t size = 0;
  CoreConfig cached;
};

ConfigCache& GetConfigCache() {
  static ConfigCache cache;
  return cache;
}

bool GetConfigStat(const std::filesystem::path& path, std::filesystem::file_time_type* mtime,
                   std::uintmax_t* size) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return false;
  }
  auto t = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return false;
  }
  auto s = std::filesystem::file_size(path, ec);
  if (ec) {
    return false;
  }
  if (mtime != nullptr) {
    *mtime = t;
  }
  if (size != nullptr) {
    *size = s;
  }
  return true;
}

void MaterializeBuiltinSceneLabels(CoreConfig* config) {
  if (config == nullptr) {
    return;
  }
  for (auto& scene : config->scenes.definitions) {
    if (!vinput::scene::IsBuiltinSceneId(scene.id)) {
      continue;
    }
    if (scene.label.empty() || vinput::scene::IsBuiltinSceneLabelKey(scene.label)) {
      scene.label = vinput::scene::DisplayLabel(scene);
    }
  }
}

// Returns false when the file exists but could not be parsed, so the caller can
// avoid caching defaults over the user's unreadable configuration.
bool LoadCoreConfigFromFile(const std::filesystem::path& path, CoreConfig* config) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }

  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  std::string parse_error;
  if (!CoreConfigFromJson(text, config, &parse_error)) {
    std::cerr << "Failed to parse vinput config: " << parse_error << "\n";
    return false;
  }
  return true;
}

} // namespace

std::string GetCoreConfigPath() {
  const auto path = vinput::path::CoreConfigPath();
  std::filesystem::path resolved;
  if (vinput::file::ResolveSymlinkPath(path, &resolved, nullptr)) {
    return resolved.string();
  }
  return path.string();
}

CoreConfig LoadCoreConfig() {
  const std::filesystem::path path = vinput::path::CoreConfigPath();

  auto& cache = GetConfigCache();
  const std::scoped_lock lock(cache.mu);

  std::filesystem::file_time_type mtime;
  std::uintmax_t size = 0;
  const bool statOk = GetConfigStat(path, &mtime, &size);
  if (statOk && cache.hasCache && cache.mtime == mtime && cache.size == size) {
    return cache.cached;
  }

  CoreConfig config;
  bool cacheable = statOk;
  if (!statOk) {
    std::string loadError;
    if (!LoadBundledDefaultCoreConfig(&config, &loadError)) {
      if (!loadError.empty()) {
        std::cerr << loadError << "\n";
      }
      return CoreConfig{};
    }
    NormalizeCoreConfig(&config);
    cacheable = false;
  } else {
    if (!LoadCoreConfigFromFile(path, &config)) {
      cacheable = false;
    }
    NormalizeCoreConfig(&config);
  }

  if (cacheable) {
    cache.cached = config;
    cache.mtime = mtime;
    cache.size = size;
    cache.hasCache = true;
  } else {
    cache.hasCache = false;
  }
  return config;
}

bool InitializeCoreConfig(std::string* error) {
  CoreConfig config;
  if (!LoadBundledDefaultCoreConfig(&config, error)) {
    return false;
  }
  if (!SaveCoreConfig(config)) {
    if (error != nullptr) {
      *error = "Failed to save default config.";
    }
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool SaveCoreConfig(const CoreConfig& config) {
  const std::filesystem::path path = vinput::path::CoreConfigPath();

  std::string error;
  if (!vinput::file::EnsureParentDirectory(path, &error)) {
    std::cerr << "Failed to create config directory: " << error << "\n";
    return false;
  }

  CoreConfig normalized = config;
  NormalizeCoreConfig(&normalized);
  if (!ValidateCoreConfig(normalized, &error)) {
    return false;
  }
  MaterializeBuiltinSceneLabels(&normalized);

  const std::string serialized = CoreConfigToJson(normalized);
  if (serialized.empty()) {
    std::cerr << "Failed to serialize vinput config\n";
    return false;
  }
  if (auto res = vinput::file::AtomicWriteTextFile(path, serialized + "\n"); !res) {
    error = res.error();
    std::cerr << "Failed to write config: " << error << "\n";
    return false;
  }

  auto& cache = GetConfigCache();
  const std::scoped_lock lock(cache.mu);

  std::filesystem::file_time_type mtime;
  std::uintmax_t size = 0;
  if (GetConfigStat(path, &mtime, &size)) {
    cache.cached = std::move(normalized);
    cache.mtime = mtime;
    cache.size = size;
    cache.hasCache = true;
  } else {
    cache.hasCache = false;
  }
  return true;
}
