#include "gui/utils/i18n_cache.h"

#include <QMetaObject>
#include <QMutexLocker>
#include <QString>
#include <QThreadPool>
#include <QtCore/QMetaObject>
#include <QtCore/QMutex>
#include <QtCore/QString>
#include <QtCore/qglobal.h>
#include <QtCore/qnamespace.h>
#include <string>
#include <utility>

#include "common/registry/registry_cache.h"
#include "common/registry/registry_i18n.h"

namespace vinput::gui {

I18nCache& I18nCache::Get() {
  static I18nCache instance;
  return instance;
}

void I18nCache::ApplyLoadedMap(vinput::registry::I18nMap map, const std::string& error,
                               quint64 generation) {
  if (!error.empty()) {
    emit reloadFailed(QString::fromStdString(error), generation);
    return;
  }
  {
    const QMutexLocker locker(&mutex_);
    map_ = std::move(map);
  }
  emit mapUpdated(generation);
}

quint64 I18nCache::LoadFromDiskSync() {
  const quint64 generation = ++generation_;
  const std::string locale = vinput::registry::DetectPreferredLocale();
  std::string error;
  auto map = vinput::registry::LoadMergedCachedI18nMap(locale, &error);
  ApplyLoadedMap(std::move(map), error, generation);
  return generation;
}

quint64 I18nCache::ReloadFromDisk() {
  const quint64 generation = ++generation_;
  QThreadPool::globalInstance()->start([this, generation]() {
    const std::string locale = vinput::registry::DetectPreferredLocale();
    std::string error;
    auto map = vinput::registry::LoadMergedCachedI18nMap(locale, &error);

    QMetaObject::invokeMethod(
        this,
        [this, generation, map = std::move(map), error = std::move(error)]() mutable {
          if (generation != generation_.load()) {
            return;
          }
          ApplyLoadedMap(std::move(map), error, generation);
        },
        Qt::QueuedConnection);
  });
  return generation;
}

vinput::registry::I18nMap I18nCache::GetMap() const {
  const QMutexLocker locker(&mutex_);
  return map_;
}

} // namespace vinput::gui
