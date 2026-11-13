#pragma once

#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qtimer.h>
#include <QtCore/qtmetamacros.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qspinbox.h>
#include <QtWidgets/qtablewidget.h>
#include <QtWidgets/qwidget.h>
#include <optional>
#include <string>
#include <unordered_set>

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

private:
  void executeDaemonAction(bool restart);
  void executeClientAction(bool start);

  struct SelectedClient {
    int row = -1;
    std::string id;
    std::string kind;
    bool auto_start = false;
    bool is_running = false;
  };
  [[nodiscard]] std::optional<SelectedClient> selectedClient() const;

  void populateAsrList(const CoreConfig& config,
                       const vinput::dbus::AsrBackendState* backend_state);
  void populateClientList();
  void refreshClientStatuses();
  void updateClientButtons();

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

  QTableWidget* tableClients_{nullptr};
  QPushButton* btnClientStart_{nullptr};
  QPushButton* btnClientStop_{nullptr};
  QCheckBox* chkClientAutoStart_{nullptr};
  bool updatingClientSelection_{false};
  std::unordered_set<std::string> pendingClientIds_;

  QLabel* lblDaemonStatus_;
  QPushButton* btnDaemonStart_;
  QPushButton* btnDaemonStop_;
  QPushButton* btnDaemonRestart_;
  QTimer* daemonRefreshTimer_ = nullptr;

signals:
  void configChanged();

private slots:
  void refreshAsrList();
  void updateAsrButtons();
  void onAsrEdit();
  void onAsrSetActive();

  void refreshDaemonStatus();
  void onDaemonStart();
  void onDaemonStop();
  void onDaemonRestart();

  void onClientStart();
  void onClientStop();
  void onClientAutoStartToggled(bool checked);

  void checkSandboxPermissions();

private:
  // Shared post-save tail of the ASR edits: refresh the list, notify listeners,
  // and reload the backend while surfacing a salvageable reload failure.
  void applyAsrConfigChange();
};

} // namespace vinput::gui
