#include "mainwindow.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtCore/qpointer.h>
#include <QtGui/qclipboard.h>
#include <QtGui/qguiapplication.h>
#include <QtWidgets/qmessagebox.h>
#include <atomic>
#include <fstream>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <optional>
#include <string_view>

#include "common/utils/downloader.h"
#include "common/utils/file_utils.h"
#include "common/utils/locale_utils.h"
#include "common/utils/path_utils.h"

#include "cli/runtime/systemd_client.h"

#include "gui/utils/config_manager.h"
#include "gui/utils/download_worker.h"
#include "gui/utils/gui_helpers.h"
#include "gui/utils/i18n_cache.h"

#include "config.h"
#include "pages/control/control_page.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include "pages/hotwords/hotword_page.h"
#endif
#include <QtCore/qnamespace.h>
#include <QtCore/qobject.h>
#include <QtCore/qobjectdefs.h>
#include <QtCore/qstring.h>
#include <QtCore/qthread.h>
#include <QtCore/qurl.h>
#include <QtGui/qdesktopservices.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qmainwindow.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qtabwidget.h>
#include <QtWidgets/qwidget.h>
#include <variant>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
#include "common/utils/version_utils.h"

#include "pages/llm/llm_page.h"
#include "pages/resources/resource_page.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), tabWidget_(new QTabWidget(this)),
      controlPage_(new vinput::gui::ControlPage(this)),
      resourcePage_(new vinput::gui::ResourcePage(this)), llmPage_(new vinput::gui::LlmPage(this))
#if VINPUT_ENABLE_LOCAL_ASR
      ,
      hotwordPage_(new vinput::gui::HotwordPage(this))
#endif
{
  setWindowTitle(tr("Vinput Configuration"));

  auto* centralWidget = new QWidget(this);
  setCentralWidget(centralWidget);
  auto* mainLayout = new QVBoxLayout(centralWidget);

  mainLayout->addWidget(tabWidget_);

  // Sync disk-only i18n preload before pages paint titles; no network here.
  vinput::gui::I18nCache::Get().LoadFromDiskSync();

  tabWidget_->addTab(controlPage_, tr("Control"));
  tabWidget_->addTab(resourcePage_, tr("Resources"));
  tabWidget_->addTab(llmPage_, tr("LLM"));
#if VINPUT_ENABLE_LOCAL_ASR
  tabWidget_->addTab(hotwordPage_, tr("Hotwords"));
#endif

  // Cross-page refresh: any config change reloads affected pages.
  connect(controlPage_, &vinput::gui::ControlPage::configChanged, this, &MainWindow::reloadAll);
  connect(resourcePage_, &vinput::gui::ResourcePage::configChanged, this, &MainWindow::reloadAll);
  connect(llmPage_, &vinput::gui::LlmPage::configChanged, this, &MainWindow::reloadAll);

  // Bottom bar
  auto* bottomLayout = new QHBoxLayout();
  auto* btnOpenConfig = new QPushButton(tr("Open Config"), this);
  connect(btnOpenConfig, &QPushButton::clicked, this, &MainWindow::onOpenConfigClicked);

  btnCheckUpdate_ = new QPushButton(tr("Check for Updates"), this);
  connect(btnCheckUpdate_, &QPushButton::clicked, this, &MainWindow::onCheckUpdateClicked);

  auto* btnSave = new QPushButton(tr("Save Settings"), this);
  connect(btnSave, &QPushButton::clicked, this, &MainWindow::onSaveClicked);

  bottomLayout->addStretch();
  bottomLayout->addWidget(btnCheckUpdate_);
  bottomLayout->addWidget(btnOpenConfig);
  bottomLayout->addWidget(btnSave);
  mainLayout->addLayout(bottomLayout);

  // Initial load after pages are constructed with a warm i18n map.
  controlPage_->reload();

  checkNotification();
}

MainWindow::~MainWindow() = default;

void MainWindow::reloadAll() {
  controlPage_->reload();
  llmPage_->reload();
}

void MainWindow::onSaveClicked() {
  CoreConfig config = vinput::gui::ConfigManager::Get().Load();

  // Save device
  const QString device = controlPage_->currentDevice();
  if (!device.isEmpty() && device != "default") {
    config.global.captureDevice = device.toStdString();
  } else {
    config.global.captureDevice.clear();
  }

  // Save audio processing settings
  config.asr.normalizeAudio = controlPage_->normalizeAudio();
  config.asr.inputGain = controlPage_->inputGain();
#if VINPUT_ENABLE_LOCAL_ASR
  config.asr.vad.enabled = controlPage_->vadEnabled();
#endif
  config.global.duckOutputWhileRecording = controlPage_->duckOutputEnabled();
  config.global.duckOutputVolume = controlPage_->duckOutputVolume();

#if VINPUT_ENABLE_LOCAL_ASR
  // Save hotwords
  const QString hotwordsFile = hotwordPage_->hotwordsFilePath();
  for (auto& prov : config.asr.providers) {
    if (auto* local = std::get_if<LocalAsrProvider>(&prov)) {
      if (!hotwordsFile.isEmpty()) {
        local->hotwordsFile = hotwordsFile.toStdString();
      } else {
        local->hotwordsFile.clear();
      }
    }
  }
  if (!hotwordsFile.isEmpty()) {
    // Write hotwords content to file
    if (!vinput::file::AtomicWriteTextFile(hotwordsFile.toStdString(),
                                           hotwordPage_->hotwordsContent().toStdString())) {
      QMessageBox::critical(this, tr("Error"), tr("Failed to save hotwords file."));
      return;
    }
  }
#endif

  if (!vinput::gui::ConfigManager::Get().Save(config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }

  const auto result = vinput::cli::SystemctlRestartWithDiagnostics();
  if (!result.ok()) {
    QMessageBox::critical(this, tr("Error"), QString::fromStdString(result.failure_message));
    return;
  }
  QMessageBox::information(this, tr("Success"), tr("Settings saved successfully!"));
  close();
}

void MainWindow::onOpenConfigClicked() {
  const auto configPath = vinput::path::CoreConfigPath();
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(configPath.string())));
}

namespace {
std::optional<std::string_view> FindJsonString(const glz::json_t& node, std::string_view key) {
  const auto* entry = vinput::json::Find(node, key);
  if (entry == nullptr) {
    return std::nullopt;
  }
  if (const auto* text = entry->get_if<std::string>()) {
    return *text;
  }
  return std::nullopt;
}

std::optional<std::string_view> FindFirstJsonString(const glz::json_t& node) {
  const auto* members = node.get_if<glz::json_t::object_t>();
  if (members == nullptr) {
    return std::nullopt;
  }
  for (const auto& [key, value] : *members) {
    (void)key;
    if (const auto* text = value.get_if<std::string>()) {
      return *text;
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> ResolveLocalizedText(const glz::json_t& node) {
  const std::string locale =
      vinput::locale::NormalizeLocale(QLocale::system().name().toStdString());
  if (const auto text = FindJsonString(node, locale); text.has_value()) {
    return text;
  }
  const auto lang = std::string(vinput::locale::ExtractLanguageCode(locale));
  if (lang != locale) {
    if (const auto text = FindJsonString(node, lang); text.has_value()) {
      return text;
    }
  }
  if (const auto text = FindJsonString(node, "en"); text.has_value()) {
    return text;
  }
  return FindFirstJsonString(node);
}

QString localizedString(const glz::json_t* node) {
  if (node == nullptr) {
    return {};
  }
  if (const auto* text = node->get_if<std::string>()) {
    return QString::fromStdString(*text);
  }
  if (!node->is_object()) {
    return {};
  }
  if (const auto text = ResolveLocalizedText(*node); text.has_value()) {
    return QString::fromUtf8(text->data(), static_cast<int>(text->size()));
  }
  return {};
}

int ReadLastNotificationId() {
  const auto read_path = vinput::path::ReadNotificationsPath();
  int local_id = 0;
  std::ifstream ifs(read_path);
  if (ifs) {
    ifs >> local_id;
  }
  return local_id;
}

struct NotificationPayload {
  int id{0};
  QString version;
  QString title;
  QString text;
  QString url;
};

std::optional<std::string> FetchNotificationJson(const std::atomic<bool>* cancel_token) {
  const std::vector<std::string> urls = {
      "https://raw.githubusercontent.com/xifan2333/fcitx5-vinput/main/"
      "notification.json"};
  std::string content;
  vinput::download::Options opts;
  opts.timeout_seconds = 5;
  opts.cancel_token = cancel_token;
  if (!vinput::download::DownloadText(urls, opts, &content)) {
    return std::nullopt;
  }
  return content;
}

std::optional<NotificationPayload> ParseNotificationPayload(std::string_view content) {
  glz::json_t j;
  if (bool(glz::read_json(j, content))) {
    return std::nullopt;
  }

  const int remote_id = vinput::json::Int(j, "id");
  if (remote_id <= 0) {
    return std::nullopt;
  }

  std::string version = vinput::json::String(j, "version");
  if (version.empty()) {
    version = vinput::json::String(j, "latest_version");
  }

  return NotificationPayload{
      .id = remote_id,
      .version = QString::fromStdString(version),
      .title = localizedString(vinput::json::Find(j, "title")),
      .text = localizedString(vinput::json::Find(j, "text")),
      .url = QString::fromStdString(vinput::json::String(j, "url")),
  };
}

bool IsNewerVersion(const QString& remote_ver_str) {
  if (remote_ver_str.isEmpty()) {
    return false;
  }
  const auto remote_ver = vinput::version::Parse(remote_ver_str.toStdString());
  const auto current_ver = vinput::version::Parse(vinput::version::CurrentHostVersion());
  if (!remote_ver.has_value() || !current_ver.has_value()) {
    return false;
  }
  return *remote_ver > *current_ver;
}

bool ShouldNotifyUpdate(const NotificationPayload& payload, int last_read_id) {
  if (payload.id <= last_read_id) {
    return false;
  }
  if (!payload.version.isEmpty() && !IsNewerVersion(payload.version)) {
    return false;
  }
  return true;
}

void DispatchNotificationResult(MainWindow* window, bool manual,
                                const std::optional<NotificationPayload>& payload, bool fetch_ok) {
  if (window == nullptr) {
    return;
  }
  if (manual) {
    if (!fetch_ok || !payload.has_value()) {
      window->onManualCheckCompleted(false, false, {}, {}, {}, {});
      return;
    }
    const bool has_update = payload->version.isEmpty() || IsNewerVersion(payload->version);
    window->onManualCheckCompleted(true, has_update, QString::number(payload->id), payload->title,
                                   payload->text, payload->url);
    return;
  }

  if (fetch_ok && payload.has_value() && ShouldNotifyUpdate(*payload, ReadLastNotificationId())) {
    window->onNotificationReady(QString::number(payload->id), payload->title, payload->text,
                                payload->url);
  }
}
} // namespace

void MainWindow::onCheckUpdateClicked() {
  if (btnCheckUpdate_ != nullptr) {
    btnCheckUpdate_->setEnabled(false);
    btnCheckUpdate_->setText(tr("Checking..."));
  }
  checkNotification(/*manual=*/true);
}

void MainWindow::resetCheckUpdateButton() {
  if (btnCheckUpdate_ != nullptr) {
    btnCheckUpdate_->setEnabled(true);
    btnCheckUpdate_->setText(tr("Check for Updates"));
  }
}

void MainWindow::onManualCheckCompleted(bool success, bool has_update, const QString& id,
                                        const QString& title, const QString& text,
                                        const QString& url) {
  resetCheckUpdateButton();
  if (!success) {
    QMessageBox::warning(this, tr("Check for Updates"),
                         tr("Failed to check for updates. Please check your network connection."));
    return;
  }
  if (has_update) {
    onNotificationReady(id, title, text, url);
  } else {
    const auto current_version =
        QString::fromUtf8(vinput::version::CurrentHostVersion().data(),
                          static_cast<int>(vinput::version::CurrentHostVersion().size()));
    QMessageBox::information(this, tr("Check for Updates"),
                             tr("You are using the latest version (%1).").arg(current_version));
  }
}

void MainWindow::checkNotification(bool manual) {
  const QPointer<MainWindow> self(this);
  auto* worker = new vinput::gui::DownloadWorker(this);
  worker->SetTask(
      [self, manual](std::string* /*error*/, const std::atomic<bool>* cancel_token) -> bool {
        const auto content = FetchNotificationJson(cancel_token);
        const bool fetch_ok = content.has_value() && !vinput::gui::IsWorkerCancelled(cancel_token);
        const auto payload = fetch_ok ? ParseNotificationPayload(*content) : std::nullopt;

        if (vinput::gui::IsWorkerCancelled(cancel_token) || self.isNull()) {
          return true;
        }

        QMetaObject::invokeMethod(
            self.data(),
            [self, manual, payload, fetch_ok]() {
              DispatchNotificationResult(self.data(), manual, payload, fetch_ok);
            },
            Qt::QueuedConnection);

        return true;
      });
  connect(worker, &QThread::finished, worker, &QObject::deleteLater);
  worker->start();
}

void MainWindow::onNotificationReady(const QString& id, const QString& title, const QString& text,
                                     const QString& url) {
  QMessageBox msgBox(this);
  msgBox.setWindowTitle(title);
  msgBox.setText(text);
  msgBox.setIcon(QMessageBox::Information);
  auto* copyBtn = msgBox.addButton(tr("Copy"), QMessageBox::ActionRole);
  auto* detailBtn =
      url.isEmpty() ? nullptr : msgBox.addButton(tr("Details"), QMessageBox::ActionRole);
  msgBox.addButton(QMessageBox::Ok);
  msgBox.exec();

  if (copyBtn != nullptr && msgBox.clickedButton() == copyBtn) {
    QGuiApplication::clipboard()->setText(text);
  } else if (detailBtn != nullptr && msgBox.clickedButton() == detailBtn) {
    QDesktopServices::openUrl(QUrl(url));
  }

  // Mark as read
  const auto read_path = vinput::path::ReadNotificationsPath();
  (void)vinput::file::AtomicWriteTextFile(read_path, id.toStdString());
}
