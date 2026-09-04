#include "pages/control/control_page.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPointer>
#include <QSpinBox>
#include <QThreadPool>
#include <QVBoxLayout>

#include "common/audio/pipewire_device.h"
#include "common/llm/adapter_manager.h"
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
    if (error) {
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
    if (error) {
      *error = daemon_error;
    }
    return false;
  }
  return dbus.GetAsrBackendState(state, error);
}

template <typename Callback> void RunReloadAsrBackendAsync(ControlPage* page, Callback callback) {
  QPointer<ControlPage> self(page);
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
  QPointer<ControlPage> self(page);
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

template <typename Callback>
void RunAdapterControlAsync(ControlPage* page, std::string adapter_id, bool start,
                            Callback callback) {
  QPointer<ControlPage> self(page);
  QThreadPool::globalInstance()->start(
      [self, adapter_id = std::move(adapter_id), start, callback = std::move(callback)]() mutable {
        vinput::cli::DbusClient dbus;
        std::string err;
        const bool ok =
            start ? dbus.StartAdapter(adapter_id, &err) : dbus.StopAdapter(adapter_id, &err);
        QMetaObject::invokeMethod(self, [self, callback = std::move(callback), ok, err]() mutable {
          if (!self) {
            return;
          }
          callback(ok, err);
        });
      });
}

} // namespace

ControlPage::ControlPage(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);

  // Audio section: capture device + audio processing knobs.
  auto* audioFrame = new QFrame();
  audioFrame->setFrameShape(QFrame::StyledPanel);
  auto* audioLayout = new QVBoxLayout(audioFrame);
  audioLayout->addWidget(new QLabel(tr("<b>Audio</b>")));

  auto* formLayout = new QFormLayout();
  comboDevice_ = new QComboBox();
  comboDevice_->setEditable(false);
  formLayout->addRow(tr("Capture Device:"), comboDevice_);

  chkNormalizeAudio_ = new QCheckBox(tr("Normalize audio"));
  chkNormalizeAudio_->setToolTip(
      tr("Apply peak normalization to captured audio before recognition."));
  formLayout->addRow(QString(), chkNormalizeAudio_);

  spinInputGain_ = new QDoubleSpinBox();
  spinInputGain_->setRange(0.1, 10.0);
  spinInputGain_->setSingleStep(0.1);
  spinInputGain_->setDecimals(1);
  spinInputGain_->setToolTip(tr("Microphone gain multiplier applied to captured audio."));
  formLayout->addRow(tr("Input gain:"), spinInputGain_);

  chkVadEnabled_ = new QCheckBox(tr("Trim silence (VAD)"));
  chkVadEnabled_->setToolTip(tr("Use voice activity detection to trim leading/trailing silence."));
  formLayout->addRow(QString(), chkVadEnabled_);

  chkDuckOutput_ = new QCheckBox(tr("Reduce output volume while recording"));
  chkDuckOutput_->setToolTip(tr("Lower the system output volume while recording, then restore it. "
                                "Useful when speaker sound leaks into the microphone."));
  formLayout->addRow(QString(), chkDuckOutput_);

  spinDuckVolume_ = new QSpinBox();
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
  listAsrProviders_ = new QListWidget();
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

  // Adapters section
  auto* adapterFrame = new QFrame();
  adapterFrame->setFrameShape(QFrame::StyledPanel);
  auto* adapterLayout = new QVBoxLayout(adapterFrame);

  auto* adapterTitle = new QLabel(tr("<b>Adapters</b>"));
  adapterLayout->addWidget(adapterTitle);

  auto* adapterListLayout = new QHBoxLayout();
  listAdapters_ = new QListWidget();
  adapterListLayout->addWidget(listAdapters_);

  auto* adapterBtnLayout = new QVBoxLayout();
  btnAdapterStart_ = new QPushButton(tr("Start"));
  btnAdapterStop_ = new QPushButton(tr("Stop"));
  chkAdapterAutostart_ = new QCheckBox(tr("Start with daemon"));
  chkAdapterAutostart_->setToolTip(
      tr("Automatically start this adapter when vinput-daemon starts."));
  adapterBtnLayout->addWidget(btnAdapterStart_);
  adapterBtnLayout->addWidget(btnAdapterStop_);
  adapterBtnLayout->addWidget(chkAdapterAutostart_);
  adapterBtnLayout->addStretch();
  adapterListLayout->addLayout(adapterBtnLayout);
  adapterLayout->addLayout(adapterListLayout);
  layout->addWidget(adapterFrame);

  connect(btnAdapterStart_, &QPushButton::clicked, this, &ControlPage::onAdapterStart);
  connect(btnAdapterStop_, &QPushButton::clicked, this, &ControlPage::onAdapterStop);
  connect(chkAdapterAutostart_, &QCheckBox::toggled, this, &ControlPage::onAdapterAutostartToggled);
  connect(listAdapters_, &QListWidget::itemSelectionChanged, this,
          &ControlPage::updateAdapterButtons);
  btnAdapterStart_->setEnabled(false);
  btnAdapterStop_->setEnabled(false);
  chkAdapterAutostart_->setEnabled(false);

  connect(&I18nCache::Get(), &I18nCache::mapUpdated, this, &ControlPage::refreshAdapterList);

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

  daemonRefreshTimer_ = new QTimer(this);
  connect(daemonRefreshTimer_, &QTimer::timeout, this, &ControlPage::refreshDaemonStatus);
  daemonRefreshTimer_->start(2000);

  QTimer::singleShot(0, this, &ControlPage::refreshDaemonStatus);
  QTimer::singleShot(0, this, &ControlPage::checkSandboxPermissions);
}

void ControlPage::reload() {
  comboDevice_->clear();
  comboDevice_->addItem("default", "default");

  const auto devices = vinput::pw::EnumerateAudioSources();
  CoreConfig config = ConfigManager::Get().Load();
  QString activeDevice = QString::fromStdString(config.global.captureDevice);

  chkNormalizeAudio_->setChecked(config.asr.normalizeAudio);
  spinInputGain_->setValue(config.asr.inputGain);
  chkVadEnabled_->setChecked(config.asr.vad.enabled);
  chkDuckOutput_->setChecked(config.global.duckOutputWhileRecording);
  spinDuckVolume_->setValue(static_cast<int>(config.global.duckOutputVolume * 100.0 + 0.5));
  spinDuckVolume_->setEnabled(chkDuckOutput_->isChecked());

  for (const auto& dev : devices) {
    QString name = QString::fromStdString(dev.name);
    QString desc = QString::fromStdString(dev.description);
    QString label = desc.isEmpty() ? name : desc;
    comboDevice_->addItem(label, name);
    if (name == activeDevice) {
      comboDevice_->setCurrentIndex(comboDevice_->count() - 1);
    }
  }
  if (comboDevice_->currentIndex() <= 0) {
    comboDevice_->setCurrentIndex(0);
  }

  refreshAsrList();
  refreshAdapterList();
}

QString ControlPage::currentDevice() const {
  QString val = comboDevice_->currentData().toString();
  if (val.isEmpty())
    val = comboDevice_->currentText();
  return val;
}

bool ControlPage::normalizeAudio() const {
  return chkNormalizeAudio_->isChecked();
}

double ControlPage::inputGain() const {
  return spinInputGain_->value();
}

bool ControlPage::vadEnabled() const {
  return chkVadEnabled_->isChecked();
}

bool ControlPage::duckOutputEnabled() const {
  return chkDuckOutput_->isChecked();
}

double ControlPage::duckOutputVolume() const {
  return spinDuckVolume_->value() / 100.0;
}

void ControlPage::refreshAsrList() {
  CoreConfig config = ConfigManager::Get().Load();
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
  const QString selected_id = QString::fromStdString(config.asr.activeProvider);
  const QString effective_id =
      backend_state ? QString::fromStdString(backend_state->effective_provider_id) : QString{};
  const QString target_id =
      backend_state ? QString::fromStdString(backend_state->target_provider_id) : QString{};
  const bool reload_in_progress = backend_state && backend_state->reload_in_progress;
  const bool reload_failed = backend_state && !backend_state->last_error.empty();

  const QString current_selection =
      listAsrProviders_->currentItem()
          ? listAsrProviders_->currentItem()->data(Qt::UserRole).toString()
          : QString{};

  listAsrProviders_->clear();
  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto& provider : config.asr.providers) {
#if !VINPUT_ENABLE_LOCAL_ASR
    if (std::holds_alternative<LocalAsrProvider>(provider)) {
      continue;
    }
#endif
    QString id = QString::fromStdString(AsrProviderId(provider));
    QString type = QString::fromStdString(std::string(AsrProviderType(provider)));

    std::string id_str = id.toStdString();
    QString title =
        QString::fromStdString(vinput::registry::LookupI18n(i18n_map, id_str + ".title", id_str));

    QString display = title + " [" + type + "]";
    if (const auto* local = std::get_if<LocalAsrProvider>(&provider)) {
      QString model = QString::fromStdString(local->model);
      if (!model.isEmpty()) {
        model = QString::fromStdString(
            vinput::registry::LookupI18n(i18n_map, local->model + ".title", local->model));
      }
      display += " · " + (model.isEmpty() ? GuiTranslate("(not set)") : model);
    } else if (const auto* command = std::get_if<CommandAsrProvider>(&provider)) {
      display += " · " + QString::fromStdString(command->command);
    }

    QStringList markers;
    if (id == selected_id) {
      markers << tr("configured");
    }
    if (id == effective_id) {
      markers << tr("effective");
    }
    if (reload_in_progress && id == target_id && id != effective_id) {
      markers << tr("loading");
    }
    if (reload_failed && id == target_id) {
      markers << tr("reload failed");
    }
    if (!markers.isEmpty()) {
      display += " · " + markers.join(", ");
    }
    if (id == effective_id && backend_state && !backend_state->remote_endpoints.empty()) {
      QStringList urls;
      for (const auto& endpoint : backend_state->remote_endpoints) {
        urls << QString::fromStdString(endpoint);
      }
      display += " · " + urls.join(" / ");
    }

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
  if (!item) {
    btnAsrEdit_->setEnabled(false);
    btnAsrSetActive_->setEnabled(false);
    return;
  }
  const bool is_local = item->data(Qt::UserRole + 1).toString() == "local";
  btnAsrEdit_->setEnabled(!is_local);
  btnAsrSetActive_->setEnabled(true);
}

void ControlPage::onAsrEdit() {
  auto* item = listAsrProviders_->currentItem();
  if (!item)
    return;
  QString provider_id = item->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  AsrProviderData current;
  bool found = false;

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
      found = true;
      break;
    }
  }

  if (!found)
    return;

  AsrProviderData updated;
  if (!ShowAsrProviderDialog(this, tr("Edit ASR Provider"), &current, &updated)) {
    return;
  }

  // Remove old
  auto it = std::remove_if(config.asr.providers.begin(), config.asr.providers.end(),
                           [&](const AsrProvider& p) { return AsrProviderId(p) == current.id; });

  config.asr.providers.erase(it, config.asr.providers.end());

  // Add new
  if (updated.type == "local") {
    LocalAsrProvider p;
    p.model = updated.model;
    p.timeoutMs = updated.timeout_ms;
    config.asr.providers.push_back(p);
  } else {
    CommandAsrProvider p;
    p.id = updated.id;
    p.command = updated.command;
    p.args = updated.args;
    p.env = updated.env;
    p.timeoutMs = updated.timeout_ms;
    config.asr.providers.push_back(p);
  }

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

void ControlPage::onAsrSetActive() {
  auto* item = listAsrProviders_->currentItem();
  if (!item)
    return;

  QString provider_id = item->data(Qt::UserRole).toString();

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

void ControlPage::refreshAdapterList() {
  const QString current_selection =
      listAdapters_->currentItem() ? listAdapters_->currentItem()->data(Qt::UserRole).toString()
                                   : QString{};

  listAdapters_->clear();
  CoreConfig config = ConfigManager::Get().Load();
  const auto i18n_map = I18nCache::Get().GetMap();

  for (const auto& adapter : config.llm.adapters) {
    QString id = QString::fromStdString(adapter.id);
    QString title = QString::fromStdString(
        vinput::registry::LookupI18n(i18n_map, adapter.id + ".title", adapter.id));
    bool running = vinput::adapter::IsRunning(adapter.id);

    QString display = title + " · " + (running ? GuiTranslate("running") : GuiTranslate("stopped"));
    display += " · " + (adapter.autoStart ? tr("autostart") : tr("manual"));
    QString command = QString::fromStdString(adapter.command);
    if (!command.isEmpty()) {
      display += " · " + command;
    }

    auto* item = new QListWidgetItem(display, listAdapters_);
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, running);
    item->setData(Qt::UserRole + 2, title);
    item->setData(Qt::UserRole + 3, adapter.autoStart);

    QString tooltip;
    if (!command.isEmpty()) {
      tooltip += "\n" + tr("Command: %1").arg(command);
    }
    if (!adapter.args.empty()) {
      QStringList argsList;
      for (const auto& a : adapter.args)
        argsList << QString::fromStdString(a);
      tooltip += "\n" + tr("Args: %1").arg(argsList.join(" "));
    }
    if (!adapter.env.empty()) {
      QStringList envList;
      for (const auto& [k, v] : adapter.env)
        envList << QString::fromStdString(k) + "=" + QString::fromStdString(v);
      tooltip += "\n" + tr("Env: %1").arg(envList.join(" "));
    }
    item->setToolTip(tooltip.trimmed());

    if (id == current_selection) {
      listAdapters_->setCurrentItem(item);
    }
  }
  updateAdapterButtons();
}

void ControlPage::updateAdapterButtons() {
  auto* item = listAdapters_->currentItem();
  if (!item) {
    btnAdapterStart_->setEnabled(false);
    btnAdapterStop_->setEnabled(false);
    chkAdapterAutostart_->setEnabled(false);
    chkAdapterAutostart_->blockSignals(true);
    chkAdapterAutostart_->setChecked(false);
    chkAdapterAutostart_->blockSignals(false);
    return;
  }
  const bool running = item->data(Qt::UserRole + 1).toBool();
  const bool autostart = item->data(Qt::UserRole + 3).toBool();
  btnAdapterStart_->setEnabled(!running);
  btnAdapterStop_->setEnabled(running);
  chkAdapterAutostart_->setEnabled(true);
  chkAdapterAutostart_->blockSignals(true);
  chkAdapterAutostart_->setChecked(autostart);
  chkAdapterAutostart_->blockSignals(false);
}

void ControlPage::onAdapterStart() {
  auto* item = listAdapters_->currentItem();
  if (!item) {
    return;
  }
  const QString adapter_id = item->data(Qt::UserRole).toString();

  btnAdapterStart_->setEnabled(false);
  btnAdapterStop_->setEnabled(false);
  RunAdapterControlAsync(this, adapter_id.toStdString(), true,
                         [this](bool ok, const std::string& err) {
                           btnAdapterStart_->setEnabled(true);
                           btnAdapterStop_->setEnabled(true);
                           if (!ok) {
                             QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
                             return;
                           }
                           refreshAdapterList();
                         });
}

void ControlPage::onAdapterStop() {
  auto* item = listAdapters_->currentItem();
  if (!item) {
    return;
  }
  const QString adapter_id = item->data(Qt::UserRole).toString();

  btnAdapterStart_->setEnabled(false);
  btnAdapterStop_->setEnabled(false);
  RunAdapterControlAsync(this, adapter_id.toStdString(), false,
                         [this](bool ok, const std::string& err) {
                           btnAdapterStart_->setEnabled(true);
                           btnAdapterStop_->setEnabled(true);
                           if (!ok) {
                             QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
                             return;
                           }
                           refreshAdapterList();
                         });
}

void ControlPage::onAdapterAutostartToggled(bool checked) {
  auto* item = listAdapters_->currentItem();
  if (!item) {
    return;
  }
  const QString adapter_id = item->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  auto it =
      std::find_if(config.llm.adapters.begin(), config.llm.adapters.end(),
                   [&adapter_id](const LlmAdapter& a) { return a.id == adapter_id.toStdString(); });
  if (it == config.llm.adapters.end()) {
    return;
  }

  if (it->autoStart == checked) {
    return;
  }

  it->autoStart = checked;
  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  refreshAdapterList();
  emit configChanged();
}

void ControlPage::refreshDaemonStatus() {
  refreshAdapterList();
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

void ControlPage::onDaemonStart() {
  btnDaemonStart_->setEnabled(false);
  btnDaemonStop_->setEnabled(false);
  btnDaemonRestart_->setEnabled(false);
  QPointer<ControlPage> self(this);
  QThreadPool::globalInstance()->start([self]() {
    const auto result = vinput::cli::SystemctlStartWithDiagnostics();
    QMetaObject::invokeMethod(self, [self, result]() {
      if (!self) {
        return;
      }
      if (!result.ok()) {
        vinput::cli::NotifyDaemonNotification(result.notification);
        QMessageBox::critical(self, self->tr("Error"),
                              QString::fromStdString(result.failure_message));
      }
      self->refreshDaemonStatus();
    });
  });
}

void ControlPage::onDaemonStop() {
  btnDaemonStop_->setEnabled(false);
  btnDaemonStart_->setEnabled(false);
  btnDaemonRestart_->setEnabled(false);
  QPointer<ControlPage> self(this);
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
  btnDaemonRestart_->setEnabled(false);
  btnDaemonStop_->setEnabled(false);
  btnDaemonStart_->setEnabled(false);
  QPointer<ControlPage> self(this);
  QThreadPool::globalInstance()->start([self]() {
    const auto result = vinput::cli::SystemctlRestartWithDiagnostics();
    QMetaObject::invokeMethod(self, [self, result]() {
      if (!self) {
        return;
      }
      if (!result.ok()) {
        vinput::cli::NotifyDaemonNotification(result.notification);
        QMessageBox::critical(self, self->tr("Error"),
                              QString::fromStdString(result.failure_message));
      }
      self->refreshDaemonStatus();
    });
  });
}

void ControlPage::checkSandboxPermissions() {
  auto missing = vinput::sandbox::MissingSandboxPermissions();
  if (missing.empty())
    return;

  const QString commands = QStringLiteral(
      "flatpak override --user --filesystem=xdg-run/pipewire-0 org.fcitx.Fcitx5\n"
      "flatpak override --user --filesystem=xdg-config/systemd:create org.fcitx.Fcitx5\n"
      "flatpak kill org.fcitx.Fcitx5");

  QMessageBox msg(this);
  msg.setIcon(QMessageBox::Warning);
  msg.setWindowTitle(tr("Additional Install Required"));
  msg.setText(tr("Vinput requires additional Flatpak permissions. "
                 "Run the following commands, then restart Fcitx5:\n\n%1")
                  .arg(commands));
  msg.exec();
}

} // namespace vinput::gui
