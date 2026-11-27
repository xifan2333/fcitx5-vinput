#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QtGlobal>

#include "mainwindow.h"

namespace {

QStringList TranslationSearchPaths() {
  // Prefer XDG_DATA_DIRS (if set), then fall back to standard locations,
  // finally app-relative share.
  QStringList paths;
  const QByteArray xdg = qgetenv("XDG_DATA_DIRS");
  if (!xdg.isEmpty()) {
    for (const auto& p : QString::fromUtf8(xdg).split(":")) {
      if (!p.isEmpty()) {
        paths.push_back(p);
      }
    }
  }
  const QStringList std = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
  for (const auto& p : std) {
    if (!paths.contains(p)) {
      paths.push_back(p);
    }
  }
  paths.push_back(QCoreApplication::applicationDirPath() + "/../share");
  paths.push_back(QCoreApplication::applicationDirPath());
  return paths;
}

bool TryLoadCandidatesFromRoot(QTranslator& translator, const QString& root,
                               const QStringList& candidates) {
  for (const auto& name : candidates) {
    // System path pattern
    QString path = QDir(root).filePath("fcitx5-vinput/translations/" + name + ".qm");
    if (translator.load(path)) {
      return true;
    }
    // Local path pattern (for running directly from build dir)
    path = QDir(root).filePath(name + ".qm");
    if (translator.load(path)) {
      return true;
    }
  }
  return false;
}

bool TryLoadTranslation(QTranslator& translator) {
  const QString locale = QLocale::system().name();
  const QStringList candidates = {
      locale,
      locale.left(2),
  };
  for (const auto& root : TranslationSearchPaths()) {
    if (TryLoadCandidatesFromRoot(translator, root, candidates)) {
      return true;
    }
  }
  return false;
}

} // namespace

int main(int argc, char* argv[]) {
  // Set IM environment for this process only, so Chinese input works in the GUI
  if (qEnvironmentVariableIsEmpty("QT_IM_MODULE")) {
    qputenv("QT_IM_MODULE", "fcitx");
  }
  if (qEnvironmentVariableIsEmpty("XMODIFIERS")) {
    qputenv("XMODIFIERS", "@im=fcitx");
  }

  // Defer widget styling to the active Qt platform theme and system palette.
  QApplication::setDesktopSettingsAware(true);
  const QApplication app(argc, argv);

  QTranslator translator;
  if (TryLoadTranslation(translator)) {
    QCoreApplication::installTranslator(&translator);
  }

  // Qt's own translations (qtbase_*) cover standard dialog buttons such as
  // OK/Cancel in QDialogButtonBox.
  QTranslator qtTranslator;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  const QString qt_translations_path = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
#else
  const QString qt_translations_path = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
#endif
  if (qtTranslator.load(QLocale::system(), QStringLiteral("qtbase"), QStringLiteral("_"),
                        qt_translations_path)) {
    QCoreApplication::installTranslator(&qtTranslator);
  }

  MainWindow window;
  window.resize(600, 400);
  window.show();

  return QApplication::exec();
}
