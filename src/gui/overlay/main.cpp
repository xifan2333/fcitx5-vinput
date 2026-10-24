#include <QApplication>
#include <QDBusConnection>

#include "recording_orb.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("vinput-recording-orb"));
  app.setQuitOnLastWindowClosed(false);

  if (!QDBusConnection::sessionBus().isConnected()) {
    return 1;
  }

  vinput::gui::RecordingOrb orb;
  return app.exec();
}
