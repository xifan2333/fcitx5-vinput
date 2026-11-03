#include "gui/utils/config_manager.h"

#include <QtCore/qmutex.h>
#include <QtCore/qtmetamacros.h>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"

namespace vinput::gui {

ConfigManager& ConfigManager::Get() {
  static ConfigManager instance;
  return instance;
}

CoreConfig ConfigManager::Load() {
  const QMutexLocker locker(&mutex_);
  return LoadCoreConfig();
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
