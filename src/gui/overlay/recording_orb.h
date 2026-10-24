#pragma once

#include <QDBusInterface>
#include <QTimer>
#include <QWidget>

class QPaintEvent;

namespace vinput::gui {

class RecordingOrb final : public QWidget {
public:
  explicit RecordingOrb(QWidget* parent = nullptr);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void pollDaemonStatus();
  void pollAudioLevel();
  void setRecording(bool recording);
  void positionOnPrimaryScreen();

  QDBusInterface daemon_;
  QTimer statusTimer_;
  QTimer audioLevelTimer_;
  QTimer animationTimer_;
  double level_ = 0.0;
  double phase_ = 0.0;
  bool recording_ = false;
};

} // namespace vinput::gui
