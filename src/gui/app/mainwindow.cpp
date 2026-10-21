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

#include "common/utils/downloader.h"
#include "common/utils/file_utils.h"
#include "common/utils/locale_utils.h"
#include "common/utils/path_utils.h"

#include "cli/runtime/systemd_client.h"

#include "gui/utils/config_manager.h"
#include "gui/utils/download_worker.h"
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

  auto* btnSave = new QPushButton(tr("Save Settings"), this);
  connect(btnSave, &QPushButton::clicked, this, &MainWindow::onSaveClicked);

  bottomLayout->addWidget(btnOpenConfig);
  bottomLayout->addStretch();
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
    std::string write_err;
    if (!vinput::file::AtomicWriteTextFile(hotwordsFile.toStdString(),
                                           hotwordPage_->hotwordsContent().toStdString(),
                                           &write_err)) {
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
    vinput::cli::NotifyDaemonNotification(result.notification);
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
  const std::string locale =
      vinput::locale::NormalizeLocale(QLocale::system().name().toStdString());
  if (const auto* entry = vinput::json::Find(*node, locale); entry != nullptr) {
    if (const auto* text = entry->get_if<std::string>()) {
      return QString::fromStdString(*text);
    }
  }
  const auto lang = std::string(vinput::locale::ExtractLanguageCode(locale));
  if (lang != locale) {
    if (const auto* entry = vinput::json::Find(*node, lang); entry != nullptr) {
      if (const auto* text = entry->get_if<std::string>()) {
        return QString::fromStdString(*text);
      }
    }
  }
  if (const auto* english = vinput::json::Find(*node, "en"); english != nullptr) {
    if (const auto* text = english->get_if<std::string>()) {
      return QString::fromStdString(*text);
    }
  }
  if (const auto* members = node->get_if<glz::json_t::object_t>()) {
    for (const auto& [key, value] : *members) {
      (void)key;
      if (const auto* text = value.get_if<std::string>()) {
        return QString::fromStdString(*text);
      }
    }
  }
  return {};
}
} // namespace

void MainWindow::checkNotification() {
  const QPointer<MainWindow> self(this);
  auto* worker = new vinput::gui::DownloadWorker(this);
  worker->SetTask([self](std::string* /*error*/, const std::atomic<bool>* cancel_token) -> bool {
    const std::vector<std::string> urls = {
        "https://raw.githubusercontent.com/xifan2333/fcitx5-vinput/main/"
        "notification.json"};
    std::string content;
    vinput::download::Options opts;
    opts.timeout_seconds = 5;
    opts.cancel_token = cancel_token;
    if (!vinput::download::DownloadText(urls, opts, &content)) {
      return true; // silent fail or cancelled
    }

    if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
      return true;
    }

    glz::json_t j;
    if (bool(glz::read_json(j, content))) {
      return true;
    }

    const int remote_id = vinput::json::Int(j, "id");
    if (remote_id <= 0) {
      return true;
    }

    // Read local last-read id
    const auto read_path = vinput::path::ReadNotificationsPath();
    int local_id = 0;
    {
      std::ifstream ifs(read_path);
      if (ifs) {
        ifs >> local_id;
      }
    }

    if (remote_id <= local_id) {
      return true;
    }

    const QString title = localizedString(vinput::json::Find(j, "title"));
    const QString text = localizedString(vinput::json::Find(j, "text"));
    const QString url = QString::fromStdString(vinput::json::String(j, "url"));
    const QString id = QString::number(remote_id);

    if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
      return true;
    }

    if (self.isNull()) {
      return true;
    }

    QMetaObject::invokeMethod(
        self.data(),
        [self, id, title, text, url]() {
          if (!self.isNull()) {
            self->onNotificationReady(id, title, text, url);
          }
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
  vinput::file::AtomicWriteTextFile(read_path, id.toStdString(), nullptr);
}
