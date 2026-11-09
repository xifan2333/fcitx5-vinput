#include "gui/utils/config_manager.h"

#include <QtCore/qmutex.h>
#include <QtCore/qtmetamacros.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/utils/path_utils.h"

namespace vinput::gui {

ConfigManager& ConfigManager::Get() {
  static ConfigManager instance;
  return instance;
}

CoreConfig ConfigManager::Load() {
  const QMutexLocker locker(&mutex_);
  return LoadCoreConfig();
}

bool ConfigManager::TryLoad(CoreConfig* config, std::string* error) {
  const QMutexLocker locker(&mutex_);
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "null config pointer";
    }
    return false;
  }

  const auto path = vinput::path::CoreConfigPath();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return LoadBundledDefaultCoreConfig(config, error);
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    if (error != nullptr) {
      *error = "failed to open config file";
    }
    return false;
  }

  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  if (!CoreConfigFromJson(text, config, error)) {
    return false;
  }
  NormalizeCoreConfig(config);
  return true;
}

bool ConfigManager::Save(const CoreConfig& config) {
  const QMutexLocker locker(&mutex_);
  const bool success = SaveCoreConfig(config);
  if (success) {
    emit configChanged();
  }
  return success;
}

} // namespace vinput::gui
