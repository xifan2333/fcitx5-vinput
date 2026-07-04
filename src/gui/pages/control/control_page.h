#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>

#include "common/config/core_config.h"
#include "common/dbus/dbus_interface.h"

namespace vinput::gui {

class ControlPage : public QWidget {
  Q_OBJECT

public:
  explicit ControlPage(QWidget *parent = nullptr);

  // Reload device combo and ASR list from CLI.
  void reload();

  // Current device value for saving.
  QString currentDevice() const;

  // Current audio-processing values for saving.
  bool normalizeAudio() const;
  double inputGain() const;
  bool vadEnabled() const;
  bool duckOutputEnabled() const;
  double duckOutputVolume() const;

signals:
  void configChanged();

private slots:
  void refreshAsrList();
  void updateAsrButtons();
  void onAsrEdit();
  void onAsrRemove();
  void onAsrSetActive();

  void refreshDaemonStatus();
  void onDaemonStart();
  void onDaemonStop();
  void onDaemonRestart();

  void checkSandboxPermissions();

private:
  void populateAsrList(const CoreConfig &config,
                       const vinput::dbus::AsrBackendState *backend_state);

  QComboBox *comboDevice_;
  QCheckBox *chkNormalizeAudio_;
  QDoubleSpinBox *spinInputGain_;
  QCheckBox *chkVadEnabled_;
  QCheckBox *chkDuckOutput_;
  QSpinBox *spinDuckVolume_;
  QListWidget *listAsrProviders_;
  QPushButton *btnAsrEdit_;
  QPushButton *btnAsrRemove_;
  QPushButton *btnAsrSetActive_;

  QLabel *lblDaemonStatus_;
  QPushButton *btnDaemonStart_;
  QPushButton *btnDaemonStop_;
  QPushButton *btnDaemonRestart_;
  QTimer *daemonRefreshTimer_ = nullptr;
};

}  // namespace vinput::gui
