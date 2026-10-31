#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>

#include "common/config/core_config.h"
#include "common/dbus/dbus_interface.h"

#include "config.h"

namespace vinput::gui {

class ControlPage : public QWidget {
  Q_OBJECT

public:
  explicit ControlPage(QWidget* parent = nullptr);

  // Reload device combo and ASR list from CLI.
  void reload();

  // Current device value for saving.
  QString currentDevice() const;

  // Current audio-processing values for saving.
  bool normalizeAudio() const;
  double inputGain() const;
#if VINPUT_ENABLE_LOCAL_ASR
  bool vadEnabled() const;
#endif
  bool duckOutputEnabled() const;
  double duckOutputVolume() const;

signals:
  void configChanged();

private slots:
  void refreshAsrList();
  void updateAsrButtons();
  void onAsrEdit();
  void onAsrSetActive();

  void refreshAdapterList();
  void updateAdapterButtons();
  void onAdapterStart();
  void onAdapterStop();
  void onAdapterAutostartToggled(bool checked);

  void refreshDaemonStatus();
  void onDaemonStart();
  void onDaemonStop();
  void onDaemonRestart();

  void checkSandboxPermissions();

private:
  void populateAsrList(const CoreConfig& config,
                       const vinput::dbus::AsrBackendState* backend_state);

  QComboBox* comboDevice_;
  QCheckBox* chkNormalizeAudio_;
  QDoubleSpinBox* spinInputGain_;
#if VINPUT_ENABLE_LOCAL_ASR
  QCheckBox* chkVadEnabled_;
#endif
  QCheckBox* chkDuckOutput_;
  QSpinBox* spinDuckVolume_;
  QListWidget* listAsrProviders_;
  QPushButton* btnAsrEdit_;
  QPushButton* btnAsrSetActive_;

  QListWidget* listAdapters_;
  QPushButton* btnAdapterStart_;
  QPushButton* btnAdapterStop_;
  QCheckBox* chkAdapterAutostart_;

  QLabel* lblDaemonStatus_;
  QPushButton* btnDaemonStart_;
  QPushButton* btnDaemonStop_;
  QPushButton* btnDaemonRestart_;
  QTimer* daemonRefreshTimer_ = nullptr;
};

} // namespace vinput::gui
