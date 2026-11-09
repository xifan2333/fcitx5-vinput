#pragma once

#include <QtCore/qmutex.h>
#include <QtCore/qobject.h>
#include <QtCore/qobjectdefs.h>
#include <QtCore/qtmetamacros.h>
#include <string>

#include "common/config/core_config_types.h"

namespace vinput::gui {

class ConfigManager : public QObject {
  Q_OBJECT
public:
  static ConfigManager& Get();

  ConfigManager(const ConfigManager&) = delete;
  ConfigManager& operator=(const ConfigManager&) = delete;
  ConfigManager(ConfigManager&&) = delete;
  ConfigManager& operator=(ConfigManager&&) = delete;

  ~ConfigManager() override = default;

  CoreConfig Load();
  bool TryLoad(CoreConfig* config, std::string* error = nullptr);
  bool Save(const CoreConfig& config);

signals:
  void configChanged();

private:
  ConfigManager() = default;

  QMutex mutex_;
};

} // namespace vinput::gui
