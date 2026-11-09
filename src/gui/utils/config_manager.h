#pragma once

#include <QMutex>
#include <QObject>
#include <string>

#include "common/config/core_config.h"

namespace vinput::gui {

class ConfigManager : public QObject {
  Q_OBJECT
public:
  static ConfigManager& Get();

  CoreConfig Load();
  bool TryLoad(CoreConfig* config, std::string* error = nullptr);
  bool Save(const CoreConfig& config);

signals:
  void configChanged();

private:
  ConfigManager() = default;
  ~ConfigManager() override = default;

  QMutex mutex_;
};

} // namespace vinput::gui
