#include "mainwindow.h"
#include <QApplication>
#include <QDir>
#include <QLocale>
#include <QStandardPaths>
#include <QTranslator>

namespace {

QStringList TranslationSearchPaths() {
  // Prefer XDG_DATA_DIRS (if set), then fall back to standard locations,
  // finally app-relative share.
  QStringList paths;
  const QByteArray xdg = qgetenv("XDG_DATA_DIRS");
  if (!xdg.isEmpty()) {
    for (const auto &p :
         QString::fromUtf8(xdg).split(":", Qt::SkipEmptyParts)) {
      paths.push_back(p);
    }
  }
  const QStringList std =
      QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
  for (const auto &p : std) {
    if (!paths.contains(p)) {
      paths.push_back(p);
    }
  }
  paths.push_back(QCoreApplication::applicationDirPath() + "/../share");
  paths.push_back(QCoreApplication::applicationDirPath());
  return paths;
}

bool TryLoadTranslation(QTranslator &translator, const QString &base_name) {
  const QString locale = QLocale::system().name();
  const QStringList candidates = {
      base_name + "_" + locale,
      base_name + "_" + locale.left(2),
  };
  for (const auto &root : TranslationSearchPaths()) {
    for (const auto &name : candidates) {
      // System path pattern
      QString path = QDir(root).filePath("fcitx5-vinput/i18n/" + name + ".qm");
      if (translator.load(path)) {
        return true;
      }
      // Local path pattern (for running directly from build dir)
      path = QDir(root).filePath(name + ".qm");
      if (translator.load(path)) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

int main(int argc, char *argv[]) {
  // Set IM environment for this process only, so Chinese input works in the GUI
  setenv("QT_IM_MODULE", "fcitx", 0);
  setenv("XMODIFIERS", "@im=fcitx", 0);

  QApplication app(argc, argv);

  QTranslator translator;
  if (TryLoadTranslation(translator, "vinput-gui")) {
    app.installTranslator(&translator);
  }

  MainWindow window;
  window.resize(600, 400);
  window.show();

  return app.exec();
}
