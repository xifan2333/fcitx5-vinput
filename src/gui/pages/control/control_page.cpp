#include "pages/control/control_page.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWidget>
#include <QtCore/qglobal.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qwidget.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <qboxlayout.h>
#include <qcombobox.h>
#include <qfont.h>
#include <qformlayout.h>
#include <qframe.h>
#include <qlabel.h>
#include <qlist.h>
#include <qmessagebox.h>
#include <qnamespace.h>
#include <qobjectdefs.h>
#include <qpointer.h>
#include <qspinbox.h>
#include <qthreadpool.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <variant>
#include <vector>

#include "common/audio/pipewire_device.h"
#include "common/client/client_manager.h"
#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"
#include "common/registry/registry_i18n.h"
#include "common/utils/sandbox.h"

#include "cli/runtime/dbus_client.h"
#include "cli/runtime/systemd_client.h"

#include "gui/utils/config_manager.h"
#include "gui/utils/i18n_cache.h"

#include "config.h"
#include "dialogs/asr_provider_dialog.h"
#include "utils/gui_helpers.h"

namespace vinput::gui {

namespace {

bool ReloadAsrBackend(std::string* error = nullptr) {
  vinput::cli::DbusClient dbus;
  std::string daemon_error;
  if (!dbus.IsDaemonRunning(&daemon_error)) {
    if (error != nullptr) {
      *error = daemon_error;
    }
    return daemon_error.empty();
  }
  return dbus.ReloadAsrBackend(error);
}

bool GetAsrBackendState(vinput::dbus::AsrBackendState* state, std::string* error = nullptr) {
  vinput::cli::DbusClient dbus;
  std::string daemon_error;
  if (!dbus.IsDaemonRunning(&daemon_error)) {
    if (error != nullptr) {
      *error = daemon_error;
    }
    return false;
  }
  return dbus.GetAsrBackendState(state, error);
}

template <typename Callback> void RunReloadAsrBackendAsync(ControlPage* page, Callback callback) {
  const QPointer<ControlPage> self(page);
  QThreadPool::globalInstance()->start([self, callback = std::move(callback)]() mutable {
    std::string err;
    const bool ok = ReloadAsrBackend(&err);
    QMetaObject::invokeMethod(self, [self, callback = std::move(callback), ok, err]() mutable {
      if (!self) {
        return;
      }
      callback(ok, err);
    });
  });
}

template <typename Callback> void RunGetAsrBackendStateAsync(ControlPage* page, Callback callback) {
  const QPointer<ControlPage> self(page);
  QThreadPool::globalInstance()->start([self, callback = std::move(callback)]() mutable {
    vinput::dbus::AsrBackendState state;
    std::string err;
    const bool ok = GetAsrBackendState(&state, &err);
    QMetaObject::invokeMethod(self,
                              [self, callback = std::move(callback), ok, state, err]() mutable {
                                if (!self) {
                                  return;
                                }
                                callback(ok, state, err);
                              });
  });
}

bool CallClientLifecycleRpc(const std::string& client_id, bool start, std::string* error) {
  vinput::cli::DbusClient dbus;
  return start ? dbus.StartClient(client_id, error) : dbus.StopClient(client_id, error);
}

template <typename Callback>
void RunClientActionAsync(ControlPage* page, const std::string& client_id, bool start,
                          Callback callback) {
  const QPointer<ControlPage> self(page);
  QThreadPool::globalInstance()->start(
      [self, client_id, start, callback = std::move(callback)]() mutable {
        std::string err;
        const bool ok = CallClientLifecycleRpc(client_id, start, &err);
        QMetaObject::invokeMethod(self,
                                  [self, callback = std::move(callback), ok, err, start]() mutable {
                                    if (!self) {
                                      return;
                                    }
                                    callback(ok, err, start);
                                  });
      });
}

struct AsrDisplayContext {
  QString selected_id;
  QString effective_id;
  QString target_id;
  bool reload_in_progress = false;
  bool reload_failed = false;
  const vinput::dbus::AsrBackendState* backend_state = nullptr;
  vinput::registry::I18nMap i18n_map;
};

QString FormatProviderDetail(const AsrProvider& provider,
                             const vinput::registry::I18nMap& i18n_map) {
  if (const auto* local = std::get_if<LocalAsrProvider>(&provider)) {
    if (local->model.empty()) {
      return ControlPage::tr("(not set)");
    }
    const auto title =
        vinput::registry::LookupI18n(i18n_map, local->model + ".title", local->model);
    return QString::fromStdString(title);
  }
  if (const auto* command = std::get_if<CommandAsrProvider>(&provider)) {
    return QString::fromStdString(command->command);
  }
  return {};
}

QList<QString> BuildProviderMarkers(const QString& id, const AsrDisplayContext& ctx) {
  QList<QString> markers;
  if (id == ctx.selected_id) {
    markers << ControlPage::tr("configured");
  }
  if (id == ctx.effective_id) {
    markers << ControlPage::tr("effective");
  }
  if (ctx.reload_in_progress && id == ctx.target_id && id != ctx.effective_id) {
    markers << ControlPage::tr("loading");
  }
  if (ctx.reload_failed && id == ctx.target_id) {
    markers << ControlPage::tr("reload failed");
  }
  return markers;
}

QString FormatRemoteEndpoints(const vinput::dbus::AsrBackendState* backend_state) {
  if (backend_state == nullptr || backend_state->remote_endpoints.empty()) {
    return {};
  }
  QList<QString> urls;
  urls.reserve(static_cast<qsizetype>(backend_state->remote_endpoints.size()));
  for (const auto& endpoint : backend_state->remote_endpoints) {
    urls << QString::fromStdString(endpoint);
  }
  return urls.join(" / ");
}

QString BuildProviderDisplay(const AsrProvider& provider, const AsrDisplayContext& ctx) {
  const QString id = QString::fromStdString(AsrProviderId(provider));
  const QString type = QString::fromStdString(std::string(AsrProviderType(provider)));
  const std::string id_str = id.toStdString();
  const QString title =
      QString::fromStdString(vinput::registry::LookupI18n(ctx.i18n_map, id_str + ".title", id_str));

  QString display = title + " [" + type + "]";
  const QString detail = FormatProviderDetail(provider, ctx.i18n_map);
  if (!detail.isEmpty()) {
    display += " · " + detail;
  }

  const auto markers = BuildProviderMarkers(id, ctx);
  if (!markers.isEmpty()) {
    display += " · " + markers.join(", ");
  }

  if (id == ctx.effective_id) {
    const QString endpoints = FormatRemoteEndpoints(ctx.backend_state);
    if (!endpoints.isEmpty()) {
      display += " · " + endpoints;
    }
  }
  return display;
}

QString FormatClientStatus(bool running, pid_t pid) {
  if (!running) {
    return ControlPage::tr("Stopped");
  }
  if (pid > 0) {
    return ControlPage::tr("Running (%1)").arg(pid);
  }
  return ControlPage::tr("Running");
}

void AppendClientRow(QTableWidget* table, const ClientSpec& client, bool is_trigger,
                     const QString& selected_id) {
  const int row = table->rowCount();
  table->insertRow(row);

  const QString id = QString::fromStdString(client.id);
  const pid_t pid = vinput::client::GetPid(client.id);
  const bool running = (pid > 0);

  auto* itemType = new QTableWidgetItem(is_trigger ? ControlPage::tr("[Trigger]")
                                                   : ControlPage::tr("[Receiver]"));
  itemType->setData(Qt::UserRole, id);
  itemType->setData(Qt::UserRole + 1, is_trigger ? "trigger" : "receiver");
  itemType->setData(Qt::UserRole + 2, client.autoStart);
  itemType->setData(Qt::UserRole + 3, running);
  itemType->setData(Qt::UserRole + 4, static_cast<int>(pid));

  auto* itemId = new QTableWidgetItem(id);
  auto* itemStatus = new QTableWidgetItem(FormatClientStatus(running, pid));
  auto* itemAutostart = new QTableWidgetItem(client.autoStart ? ControlPage::tr("Enabled")
                                                              : ControlPage::tr("Disabled"));

  table->setItem(row, 0, itemType);
  table->setItem(row, 1, itemId);
  table->setItem(row, 2, itemStatus);
  table->setItem(row, 3, itemAutostart);

  if (id == selected_id) {
    table->selectRow(row);
  }
}

bool UpdateClientAutostart(CoreConfig& config, std::string_view client_id, std::string_view kind,
                           bool checked) {
  auto& list = (kind == "trigger") ? config.triggers : config.receivers;
  for (auto& client : list) {
    if (client.id == client_id) {
      client.autoStart = checked;
      return true;
    }
  }
  return false;
}

} // namespace

ControlPage::ControlPage(QWidget* parent)
    : QWidget(parent), comboDevice_(new QComboBox()), spinInputGain_(new QDoubleSpinBox()),
      spinDuckVolume_(new QSpinBox()), listAsrProviders_(new QListWidget()),
      tableClients_(new QTableWidget(this)), daemonRefreshTimer_(new QTimer(this)) {
  auto* layout = new QVBoxLayout(this);

  // Audio section: capture device + audio processing knobs.
  auto* audioFrame = new QFrame();
  audioFrame->setFrameShape(QFrame::StyledPanel);
  auto* audioLayout = new QVBoxLayout(audioFrame);
  audioLayout->addWidget(new QLabel(tr("<b>Audio</b>")));

  auto* formLayout = new QFormLayout();
  formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  comboDevice_->setEditable(false);
  formLayout->addRow(tr("Capture Device:"), comboDevice_);

  chkNormalizeAudio_ = new QCheckBox(tr("Normalize audio"));
  chkNormalizeAudio_->setToolTip(
      tr("Apply peak normalization to captured audio before recognition."));
  formLayout->addRow(QString(), chkNormalizeAudio_);

  spinInputGain_->setRange(0.1, 10.0);
  spinInputGain_->setSingleStep(0.1);
  spinInputGain_->setDecimals(1);
  spinInputGain_->setToolTip(tr("Microphone gain multiplier applied to captured audio."));
  formLayout->addRow(tr("Input gain:"), spinInputGain_);

#if VINPUT_ENABLE_LOCAL_ASR
  chkVadEnabled_ = new QCheckBox(tr("Trim silence (VAD)"));
  chkVadEnabled_->setToolTip(tr("Use voice activity detection to trim leading/trailing silence."));
  formLayout->addRow(QString(), chkVadEnabled_);
#endif

  chkDuckOutput_ = new QCheckBox(tr("Reduce output volume while recording"));
  chkDuckOutput_->setToolTip(tr("Lower the system output volume while recording, then restore it. "
                                "Useful when speaker sound leaks into the microphone."));
  formLayout->addRow(QString(), chkDuckOutput_);

  spinDuckVolume_->setRange(0, 100);
  spinDuckVolume_->setSuffix(tr(" %"));
  spinDuckVolume_->setToolTip(tr("Fraction of the current output volume to keep while recording."));
  formLayout->addRow(tr("Output volume while recording:"), spinDuckVolume_);

  connect(chkDuckOutput_, &QCheckBox::toggled, spinDuckVolume_, &QWidget::setEnabled);
  spinDuckVolume_->setEnabled(chkDuckOutput_->isChecked());

  audioLayout->addLayout(formLayout);
  layout->addWidget(audioFrame);

  layout->addSpacing(20);

  // ASR Providers section
  auto* asrFrame = new QFrame();
  asrFrame->setFrameShape(QFrame::StyledPanel);
  auto* asrLayout = new QVBoxLayout(asrFrame);

  auto* asrTitle = new QLabel(tr("<b>ASR Providers</b>"));
  asrLayout->addWidget(asrTitle);

  auto* asrListLayout = new QHBoxLayout();

  asrListLayout->addWidget(listAsrProviders_);

  auto* asrBtnLayout = new QVBoxLayout();
  btnAsrEdit_ = new QPushButton(tr("Edit"));
  btnAsrSetActive_ = new QPushButton(tr("Activate"));
  asrBtnLayout->addWidget(btnAsrEdit_);
  asrBtnLayout->addWidget(btnAsrSetActive_);
  asrBtnLayout->addStretch();
  asrListLayout->addLayout(asrBtnLayout);
  asrLayout->addLayout(asrListLayout);
  layout->addWidget(asrFrame);

  connect(btnAsrEdit_, &QPushButton::clicked, this, &ControlPage::onAsrEdit);
  connect(btnAsrSetActive_, &QPushButton::clicked, this, &ControlPage::onAsrSetActive);
  connect(listAsrProviders_, &QListWidget::itemSelectionChanged, this,
          &ControlPage::updateAsrButtons);
  btnAsrEdit_->setEnabled(false);
  btnAsrSetActive_->setEnabled(false);

  connect(&I18nCache::Get(), &I18nCache::mapUpdated, this, &ControlPage::refreshAsrList);

  layout->addSpacing(20);

  // Client Processes section
  auto* clientFrame = new QFrame();
  clientFrame->setFrameShape(QFrame::StyledPanel);
  auto* clientLayout = new QVBoxLayout(clientFrame);

  auto* clientTitle = new QLabel(tr("<b>Client Processes</b>"));
  clientLayout->addWidget(clientTitle);

  auto* clientContentLayout = new QHBoxLayout();

  tableClients_->setColumnCount(4);
  tableClients_->setHorizontalHeaderLabels({tr("Type"), tr("ID"), tr("Status"), tr("Autostart")});
  tableClients_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  tableClients_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  tableClients_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  tableClients_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  tableClients_->horizontalHeader()->setStretchLastSection(false);
  tableClients_->verticalHeader()->setVisible(false);
  tableClients_->setSelectionBehavior(QAbstractItemView::SelectRows);
  tableClients_->setSelectionMode(QAbstractItemView::SingleSelection);
  tableClients_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  tableClients_->setMinimumHeight(100);
  tableClients_->setMaximumHeight(160);

  clientContentLayout->addWidget(tableClients_);

  auto* clientBtnLayout = new QVBoxLayout();
  btnClientStart_ = new QPushButton(tr("Start"));
  btnClientStop_ = new QPushButton(tr("Stop"));
  chkClientAutoStart_ = new QCheckBox(tr("Start with daemon"));
  chkClientAutoStart_->setToolTip(tr("Automatically start this client when the daemon starts."));

  btnClientStart_->setEnabled(false);
  btnClientStop_->setEnabled(false);
  chkClientAutoStart_->setEnabled(false);

  clientBtnLayout->addWidget(btnClientStart_);
  clientBtnLayout->addWidget(btnClientStop_);
  clientBtnLayout->addWidget(chkClientAutoStart_);
  clientBtnLayout->addStretch();
  clientContentLayout->addLayout(clientBtnLayout);
  clientLayout->addLayout(clientContentLayout);
  layout->addWidget(clientFrame);

  connect(btnClientStart_, &QPushButton::clicked, this, &ControlPage::onClientStart);
  connect(btnClientStop_, &QPushButton::clicked, this, &ControlPage::onClientStop);
  connect(chkClientAutoStart_, &QCheckBox::toggled, this, &ControlPage::onClientAutoStartToggled);
  connect(tableClients_, &QTableWidget::itemSelectionChanged, this,
          &ControlPage::updateClientButtons);

  layout->addSpacing(20);

  // Daemon section
  auto* daemonFrame = new QFrame();
  daemonFrame->setFrameShape(QFrame::StyledPanel);
  auto* daemonLayout = new QVBoxLayout(daemonFrame);

  auto* statusLayout = new QHBoxLayout();
  statusLayout->addWidget(new QLabel(tr("Daemon Status:")));
  lblDaemonStatus_ = new QLabel(tr("Unknown"));
  QFont boldFont = lblDaemonStatus_->font();
  boldFont.setBold(true);
  lblDaemonStatus_->setFont(boldFont);
  statusLayout->addWidget(lblDaemonStatus_);
  statusLayout->addStretch();
  daemonLayout->addLayout(statusLayout);

  auto* btnLayout = new QHBoxLayout();
  btnDaemonStart_ = new QPushButton(tr("Start"));
  btnDaemonStop_ = new QPushButton(tr("Stop"));
  btnDaemonRestart_ = new QPushButton(tr("Restart"));
  btnLayout->addWidget(btnDaemonStart_);
  btnLayout->addWidget(btnDaemonStop_);
  btnLayout->addWidget(btnDaemonRestart_);
  btnLayout->addStretch();
  daemonLayout->addLayout(btnLayout);
  layout->addWidget(daemonFrame);

  connect(btnDaemonStart_, &QPushButton::clicked, this, &ControlPage::onDaemonStart);
  connect(btnDaemonStop_, &QPushButton::clicked, this, &ControlPage::onDaemonStop);
  connect(btnDaemonRestart_, &QPushButton::clicked, this, &ControlPage::onDaemonRestart);

  layout->addStretch();

  connect(daemonRefreshTimer_, &QTimer::timeout, this, &ControlPage::refreshDaemonStatus);
  daemonRefreshTimer_->start(2000);

  QTimer::singleShot(0, this, &ControlPage::refreshDaemonStatus);
  QTimer::singleShot(0, this, &ControlPage::checkSandboxPermissions);
}

void ControlPage::reload() {
  comboDevice_->clear();
  comboDevice_->addItem("default", "default");

  const auto devices = vinput::pw::EnumerateAudioSources();
  const CoreConfig config = ConfigManager::Get().Load();
  const QString activeDevice = QString::fromStdString(config.global.captureDevice);

  chkNormalizeAudio_->setChecked(config.asr.normalizeAudio);
  spinInputGain_->setValue(config.asr.inputGain);
#if VINPUT_ENABLE_LOCAL_ASR
  chkVadEnabled_->setChecked(config.asr.vad.enabled);
#endif
  chkDuckOutput_->setChecked(config.global.duckOutputWhileRecording);
  spinDuckVolume_->setValue(static_cast<int>(std::lround(config.global.duckOutputVolume * 100.0)));
  spinDuckVolume_->setEnabled(chkDuckOutput_->isChecked());

  for (const auto& dev : devices) {
    const QString name = QString::fromStdString(dev.name);
    const QString desc = QString::fromStdString(dev.description);
    const QString label = desc.isEmpty() ? name : desc;
    comboDevice_->addItem(label, name);
    if (name == activeDevice) {
      comboDevice_->setCurrentIndex(comboDevice_->count() - 1);
    }
  }
  if (comboDevice_->currentIndex() <= 0) {
    comboDevice_->setCurrentIndex(0);
  }

  refreshAsrList();
  populateClientList();
}

QString ControlPage::currentDevice() const {
  QString val = comboDevice_->currentData().toString();
  if (val.isEmpty()) {
    val = comboDevice_->currentText();
  }
  return val;
}

bool ControlPage::normalizeAudio() const {
  return chkNormalizeAudio_->isChecked();
}

double ControlPage::inputGain() const {
  return spinInputGain_->value();
}

#if VINPUT_ENABLE_LOCAL_ASR
bool ControlPage::vadEnabled() const {
  return chkVadEnabled_ != nullptr && chkVadEnabled_->isChecked();
}
#endif

bool ControlPage::duckOutputEnabled() const {
  return chkDuckOutput_->isChecked();
}

double ControlPage::duckOutputVolume() const {
  return spinDuckVolume_->value() / 100.0;
}

void ControlPage::refreshAsrList() {
  const CoreConfig config = ConfigManager::Get().Load();
  populateAsrList(config, nullptr);
  RunGetAsrBackendStateAsync(
      this, [this](bool ok, const vinput::dbus::AsrBackendState& state, const std::string&) {
        if (!ok) {
          return;
        }
        populateAsrList(ConfigManager::Get().Load(), &state);
      });
}

void ControlPage::populateAsrList(const CoreConfig& config,
                                  const vinput::dbus::AsrBackendState* backend_state) {
  const AsrDisplayContext ctx{
      .selected_id = QString::fromStdString(config.asr.activeProvider),
      .effective_id = (backend_state != nullptr)
                          ? QString::fromStdString(backend_state->effective_provider_id)
                          : QString{},
      .target_id = (backend_state != nullptr)
                       ? QString::fromStdString(backend_state->target_provider_id)
                       : QString{},
      .reload_in_progress = (backend_state != nullptr) && backend_state->reload_in_progress,
      .reload_failed = (backend_state != nullptr) && !backend_state->last_error.empty(),
      .backend_state = backend_state,
      .i18n_map = I18nCache::Get().GetMap(),
  };

  const QString current_selection =
      (listAsrProviders_->currentItem() != nullptr)
          ? listAsrProviders_->currentItem()->data(Qt::UserRole).toString()
          : QString{};

  listAsrProviders_->clear();

  for (const auto& provider : config.asr.providers) {
#if !VINPUT_ENABLE_LOCAL_ASR
    if (std::holds_alternative<LocalAsrProvider>(provider)) {
      continue;
    }
#endif
    const QString id = QString::fromStdString(AsrProviderId(provider));
    const QString type = QString::fromStdString(std::string(AsrProviderType(provider)));
    const std::string id_str = id.toStdString();
    const QString title = QString::fromStdString(
        vinput::registry::LookupI18n(ctx.i18n_map, id_str + ".title", id_str));
    const QString display = BuildProviderDisplay(provider, ctx);

    auto* item = new QListWidgetItem(display, listAsrProviders_);
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, type);
    item->setData(Qt::UserRole + 2, title);
    if (id == current_selection) {
      listAsrProviders_->setCurrentItem(item);
    }
  }
}

void ControlPage::updateAsrButtons() {
  auto* item = listAsrProviders_->currentItem();
  if (item == nullptr) {
    btnAsrEdit_->setEnabled(false);
    btnAsrSetActive_->setEnabled(false);
    return;
  }
  const bool is_local = item->data(Qt::UserRole + 1).toString() == "local";
  btnAsrEdit_->setEnabled(!is_local);
  btnAsrSetActive_->setEnabled(true);
}

void ControlPage::populateClientList() {
  const QString selected_id =
      (tableClients_->currentRow() >= 0 &&
       tableClients_->item(tableClients_->currentRow(), 0) != nullptr)
          ? tableClients_->item(tableClients_->currentRow(), 0)->data(Qt::UserRole).toString()
          : QString{};

  tableClients_->setRowCount(0);
  const CoreConfig config = ConfigManager::Get().Load();

  for (const auto& trigger : config.triggers) {
    AppendClientRow(tableClients_, trigger, true, selected_id);
  }
  for (const auto& receiver : config.receivers) {
    AppendClientRow(tableClients_, receiver, false, selected_id);
  }

  updateClientButtons();
}

void ControlPage::refreshClientStatuses() {
  const int count = tableClients_->rowCount();
  for (int row = 0; row < count; ++row) {
    auto* itemType = tableClients_->item(row, 0);
    auto* itemStatus = tableClients_->item(row, 2);
    if (itemType == nullptr || itemStatus == nullptr) {
      continue;
    }
    const QString id = itemType->data(Qt::UserRole).toString();
    const pid_t pid = vinput::client::GetPid(id.toStdString());
    const bool running = (pid > 0);

    itemType->setData(Qt::UserRole + 3, running);
    itemType->setData(Qt::UserRole + 4, static_cast<int>(pid));
    itemStatus->setText(FormatClientStatus(running, pid));
  }
  updateClientButtons();
}

std::optional<ControlPage::SelectedClient> ControlPage::selectedClient() const {
  if (tableClients_->selectionModel() == nullptr ||
      tableClients_->selectionModel()->selectedRows().isEmpty()) {
    return std::nullopt;
  }
  const int row = tableClients_->currentRow();
  if (row < 0) {
    return std::nullopt;
  }
  auto* item = tableClients_->item(row, 0);
  if (item == nullptr) {
    return std::nullopt;
  }
  return SelectedClient{
      .row = row,
      .id = item->data(Qt::UserRole).toString().toStdString(),
      .kind = item->data(Qt::UserRole + 1).toString().toStdString(),
      .auto_start = item->data(Qt::UserRole + 2).toBool(),
      .is_running = item->data(Qt::UserRole + 3).toBool(),
  };
}

void ControlPage::updateClientButtons() {
  const auto client = selectedClient();
  if (!client.has_value()) {
    btnClientStart_->setEnabled(false);
    btnClientStop_->setEnabled(false);
    chkClientAutoStart_->setEnabled(false);
    updatingClientSelection_ = true;
    chkClientAutoStart_->setChecked(false);
    updatingClientSelection_ = false;
    return;
  }
  const bool is_pending = (!pendingClientId_.empty() && pendingClientId_ == client->id);
  btnClientStart_->setEnabled(!is_pending && !client->is_running);
  btnClientStop_->setEnabled(!is_pending && client->is_running);
  chkClientAutoStart_->setEnabled(true);
  updatingClientSelection_ = true;
  chkClientAutoStart_->setChecked(client->auto_start);
  updatingClientSelection_ = false;
}

void ControlPage::executeClientAction(bool start) {
  const auto client = selectedClient();
  if (!client.has_value() || (!pendingClientId_.empty() && pendingClientId_ == client->id)) {
    return;
  }
  pendingClientId_ = client->id;
  btnClientStart_->setEnabled(false);
  btnClientStop_->setEnabled(false);

  RunClientActionAsync(
      this, client->id, start, [this](bool ok, const std::string& err, bool is_start) {
        pendingClientId_.clear();
        if (!ok) {
          const QString msg =
              is_start ? tr("Failed to start client: %1") : tr("Failed to stop client: %1");
          QMessageBox::warning(this, tr("Warning"), msg.arg(QString::fromStdString(err)));
        }
        refreshClientStatuses();
      });
}

void ControlPage::onClientStart() {
  executeClientAction(true);
}

void ControlPage::onClientStop() {
  executeClientAction(false);
}

void ControlPage::onClientAutoStartToggled(bool checked) {
  if (updatingClientSelection_) {
    return;
  }
  const auto client = selectedClient();
  if (!client.has_value()) {
    return;
  }

  CoreConfig config = ConfigManager::Get().Load();
  if (!UpdateClientAutostart(config, client->id, client->kind, checked)) {
    return;
  }

  if (!ConfigManager::Get().Save(config)) {
    updatingClientSelection_ = true;
    chkClientAutoStart_->setChecked(client->auto_start);
    updatingClientSelection_ = false;
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }

  if (auto* item = tableClients_->item(client->row, 0)) {
    item->setData(Qt::UserRole + 2, checked);
  }
  if (auto* autostartItem = tableClients_->item(client->row, 3)) {
    autostartItem->setText(checked ? tr("Enabled") : tr("Disabled"));
  }
  emit configChanged();
}

void ControlPage::onAsrEdit() {
  auto* item = listAsrProviders_->currentItem();
  if (item == nullptr) {
    return;
  }
  const QString provider_id = item->data(Qt::UserRole).toString();

  const CoreConfig config = ConfigManager::Get().Load();
  AsrProviderData current;

  for (const auto& provider : config.asr.providers) {
    if (AsrProviderId(provider) == provider_id.toStdString()) {
      current.id = AsrProviderId(provider);
      current.type = std::string(AsrProviderType(provider));
      current.timeout_ms = AsrProviderTimeoutMs(provider);
      if (const auto* local = std::get_if<LocalAsrProvider>(&provider)) {
        current.model = local->model;
      } else if (const auto* cmd = std::get_if<CommandAsrProvider>(&provider)) {
        current.command = cmd->command;
        current.args = cmd->args;
        current.env = cmd->env;
      }
      break;
    }
  }

  if (current.id.empty()) {
    return;
  }

  AsrProviderData updated;
  if (!ShowAsrProviderDialog(this, tr("Edit ASR Provider"), &current, &updated)) {
    return;
  }

  CoreConfig updated_config = ConfigManager::Get().Load();
  // Remove old
  std::erase_if(updated_config.asr.providers,
                [&](const AsrProvider& p) { return AsrProviderId(p) == current.id; });

  // Add new
#if VINPUT_ENABLE_LOCAL_ASR
  if (updated.type == "local") {
    LocalAsrProvider p;
    p.model = updated.model;
    p.timeoutMs = updated.timeout_ms;
    updated_config.asr.providers.emplace_back(p);
  } else
#endif
  {
    CommandAsrProvider p;
    p.id = updated.id;
    p.command = updated.command;
    p.args = updated.args;
    p.env = updated.env;
    p.timeoutMs = updated.timeout_ms;
    updated_config.asr.providers.emplace_back(p);
  }

  if (!ConfigManager::Get().Save(updated_config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  refreshAsrList();
  emit configChanged();
  RunReloadAsrBackendAsync(this, [this](bool ok, const std::string& err) {
    if (!ok) {
      QMessageBox::warning(this, tr("Warning"),
                           tr("Config saved, but failed to reload ASR backend: %1")
                               .arg(QString::fromStdString(err)));
    }
  });
}

void ControlPage::onAsrSetActive() {
  auto* item = listAsrProviders_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString provider_id = item->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  config.asr.activeProvider = provider_id.toStdString();
  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  refreshAsrList();
  emit configChanged();
  RunReloadAsrBackendAsync(this, [this](bool ok, const std::string& err) {
    if (!ok) {
      QMessageBox::warning(this, tr("Warning"),
                           tr("Config saved, but failed to reload ASR backend: %1")
                               .arg(QString::fromStdString(err)));
    }
  });
}

void ControlPage::refreshDaemonStatus() {
  refreshClientStatuses();

  vinput::cli::DbusClient dbus;
  std::string err;
  if (!dbus.IsDaemonRunning(&err)) {
    lblDaemonStatus_->setText(tr("Stopped"));
    btnDaemonStart_->setEnabled(true);
    btnDaemonStop_->setEnabled(false);
    btnDaemonRestart_->setEnabled(false);
    return;
  }
  std::string status;
  if (!dbus.GetDaemonStatus(&status, &err)) {
    lblDaemonStatus_->setText(tr("Running (Status Error: %1)").arg(QString::fromStdString(err)));
  } else {
    vinput::dbus::AsrBackendState backend_state;
    std::string state_error;
    const bool have_backend_state = dbus.GetAsrBackendState(&backend_state, &state_error);
    if (have_backend_state && backend_state.reload_in_progress) {
      lblDaemonStatus_->setText(
          tr("Running: %1 (loading backend)").arg(QString::fromStdString(status)));
    } else if (have_backend_state && !backend_state.last_error.empty()) {
      lblDaemonStatus_->setText(
          tr("Running: %1 (backend error)").arg(QString::fromStdString(status)));
    } else {
      lblDaemonStatus_->setText(tr("Running: %1").arg(QString::fromStdString(status)));
    }
  }
  btnDaemonStart_->setEnabled(false);
  btnDaemonStop_->setEnabled(true);
  btnDaemonRestart_->setEnabled(true);
}

void ControlPage::executeDaemonAction(bool restart) {
  btnDaemonStart_->setEnabled(false);
  btnDaemonStop_->setEnabled(false);
  btnDaemonRestart_->setEnabled(false);
  const QPointer<ControlPage> self(this);
  QThreadPool::globalInstance()->start([self, restart]() {
    const auto result = restart ? vinput::cli::SystemctlRestartWithDiagnostics()
                                : vinput::cli::SystemctlStartWithDiagnostics();
    QMetaObject::invokeMethod(self, [self, result]() {
      if (!self) {
        return;
      }
      if (!result.ok()) {
        QMessageBox::critical(self, ControlPage::tr("Error"),
                              QString::fromStdString(result.failure_message));
      }
      self->refreshDaemonStatus();
    });
  });
}

void ControlPage::onDaemonStart() {
  executeDaemonAction(false);
}

void ControlPage::onDaemonStop() {
  btnDaemonStop_->setEnabled(false);
  btnDaemonStart_->setEnabled(false);
  btnDaemonRestart_->setEnabled(false);
  const QPointer<ControlPage> self(this);
  QThreadPool::globalInstance()->start([self]() {
    vinput::cli::SystemctlStop();
    QMetaObject::invokeMethod(self, [self]() {
      if (!self) {
        return;
      }
      self->refreshDaemonStatus();
    });
  });
}

void ControlPage::onDaemonRestart() {
  executeDaemonAction(true);
}

void ControlPage::checkSandboxPermissions() {
  auto missing = vinput::sandbox::MissingSandboxPermissions();
  if (missing.empty()) {
    return;
  }

  const QString commands = QString::fromStdString(vinput::sandbox::FormatOverrideCommands(missing));
  if (commands.isEmpty()) {
    return;
  }

  QMessageBox msg(this);
  msg.setIcon(QMessageBox::Warning);
  msg.setWindowTitle(tr("Additional Install Required"));
  msg.setText(tr("Vinput requires additional Flatpak permissions. "
                 "Run the following commands, then restart Fcitx5:\n\n%1")
                  .arg(commands));
  msg.exec();
}

} // namespace vinput::gui
