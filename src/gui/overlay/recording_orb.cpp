#include "recording_orb.h"

#include <algorithm>
#include <cmath>

#include <QDBusConnection>
#include <QDBusReply>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>

namespace vinput::gui {
namespace {

constexpr int kOrbWidth = 128;
constexpr int kOrbHeight = 136;

} // namespace

RecordingOrb::RecordingOrb(QWidget* parent)
    : QWidget(parent),
      daemon_(QStringLiteral("org.fcitx.Vinput"), QStringLiteral("/org/fcitx/Vinput"),
              QStringLiteral("org.fcitx.Vinput.Service"), QDBusConnection::sessionBus()) {
  setWindowTitle(QStringLiteral("VInput Recording"));
  setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                 Qt::WindowDoesNotAcceptFocus);
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setFocusPolicy(Qt::NoFocus);
  setFixedSize(kOrbWidth, kOrbHeight);

  connect(&statusTimer_, &QTimer::timeout, this, &RecordingOrb::pollDaemonStatus);
  connect(&audioLevelTimer_, &QTimer::timeout, this, &RecordingOrb::pollAudioLevel);
  connect(&animationTimer_, &QTimer::timeout, this, [this] {
    if (!recording_) {
      return;
    }
    phase_ += 0.12;
    update();
  });

  statusTimer_.start(100);
  audioLevelTimer_.start(50);
  animationTimer_.start(33);
  hide();
}

void RecordingOrb::pollDaemonStatus() {
  const QDBusReply<QString> reply = daemon_.call(QStringLiteral("GetStatus"));
  setRecording(reply.isValid() && reply.value() == QStringLiteral("recording"));
}

void RecordingOrb::pollAudioLevel() {
  if (!recording_) {
    return;
  }

  const QDBusReply<double> reply = daemon_.call(QStringLiteral("GetAudioLevel"));
  level_ = reply.isValid() ? std::clamp(reply.value(), 0.0, 1.0) : 0.0;
  update();
}

void RecordingOrb::setRecording(bool recording) {
  if (recording_ == recording) {
    return;
  }

  recording_ = recording;
  if (recording_) {
    phase_ = 0.0;
    level_ = 0.0;
    positionOnPrimaryScreen();
    show();
  } else {
    level_ = 0.0;
    hide();
  }
}

void RecordingOrb::positionOnPrimaryScreen() {
  QScreen* screen = QGuiApplication::primaryScreen();
  if (!screen) {
    return;
  }
  const QRect geometry = screen->availableGeometry();
  const int x = geometry.x() + (geometry.width() - width()) / 2;
  const int y = geometry.y() + geometry.height() - height() - 44;
  move(x, y);
}

void RecordingOrb::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  if (!recording_) {
    return;
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(rect(), Qt::transparent);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

  const QPointF center(width() / 2.0, 61.0);
  const double breath = 0.035 * (1.0 + std::sin(phase_ * 1.7));
  const double energy = std::clamp(std::max(level_, breath), 0.035, 1.0);
  const double outer_radius = 39.0 + energy * 8.0;

  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(41, 217, 187, static_cast<int>(12 + energy * 22)));
  painter.drawEllipse(center, outer_radius + 9.0, outer_radius + 9.0);

  QPen ring(QColor(72, 232, 202, static_cast<int>(78 + energy * 125)));
  ring.setWidthF(1.5 + energy * 2.0);
  painter.setPen(ring);
  painter.setBrush(Qt::NoBrush);
  painter.drawEllipse(center, outer_radius, outer_radius);

  painter.setPen(QPen(QColor(60, 184, 165, 148), 1.0));
  painter.setBrush(QColor(14, 22, 27, 246));
  painter.drawEllipse(center, 34.0, 34.0);

  constexpr int kBarCount = 7;
  constexpr double kBarWidth = 3.2;
  constexpr double kGap = 5.0;
  const double total_width = kBarCount * kBarWidth + (kBarCount - 1) * kGap;
  const double left = center.x() - total_width / 2.0;
  for (int index = 0; index < kBarCount; ++index) {
    const double ripple = 0.5 + 0.5 * std::sin(phase_ + index * 0.78);
    const double height = 5.0 + 24.0 * std::min(1.0, energy * (0.42 + ripple * 0.85));
    const double x = left + index * (kBarWidth + kGap);
    const QRectF bar(x, center.y() - height / 2.0, kBarWidth, height);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(105, 240, 210, static_cast<int>(184 + ripple * 71)));
    painter.drawRoundedRect(bar, kBarWidth / 2.0, kBarWidth / 2.0);
  }

  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(255, 83, 89, 245));
  painter.drawEllipse(center + QPointF(24.0, -24.0), 4.0, 4.0);
}

} // namespace vinput::gui
