#pragma once

#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qtimer.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qspinbox.h>
#include <QtWidgets/qwidget.h>

#include "common/config/core_config_types.h"
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
  [[nodiscard]] QString currentDevice() const;

  // Current audio-processing values for saving.
  [[nodiscard]] bool normalizeAudio() const;
  [[nodiscard]] double inputGain() const;
#if VINPUT_ENABLE_LOCAL_ASR
  [[nodiscard]] bool vadEnabled() const;
#endif
  [[nodiscard]] bool duckOutputEnabled() const;
  [[nodiscard]] double duckOutputVolume() const;

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

  void executeAdapterControl(bool start);
  void executeDaemonAction(bool restart);
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
