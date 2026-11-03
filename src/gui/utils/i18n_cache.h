#pragma once

#include <QtCore/qglobal.h>
#include <QtCore/qmutex.h>
#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qtmetamacros.h>
#include <atomic>
#include <cstdint>
#include <string>

#include "common/registry/registry_i18n.h"

namespace vinput::gui {

class I18nCache : public QObject {
  Q_OBJECT
public:
  static I18nCache& Get();
  ~I18nCache() override = default;

  // Synchronously load the merged disk i18n map on the calling thread.
  // Invalidates any in-flight async reload and emits mapUpdated on success
  // (or reloadFailed when the merged map is empty).
  quint64 LoadFromDiskSync();

  // Starts an async disk reload and returns the generation for that request.
  // Only the latest generation emits mapUpdated/reloadFailed.
  quint64 ReloadFromDisk();
  vinput::registry::I18nMap GetMap() const;

signals:
  void mapUpdated(quint64 generation);
  void reloadFailed(const QString& error, quint64 generation);

private:
  I18nCache() = default;

  void ApplyLoadedMap(vinput::registry::I18nMap map, const std::string& error, quint64 generation);

  mutable QMutex mutex_;
  vinput::registry::I18nMap map_;
  std::atomic<uint64_t> generation_{0};
};

} // namespace vinput::gui
