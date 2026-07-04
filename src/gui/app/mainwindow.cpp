#include "mainwindow.h"

#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include <fstream>

#include <nlohmann/json.hpp>

#include "pages/control/control_page.h"
#include "pages/hotwords/hotword_page.h"
#include "pages/llm/llm_page.h"
#include "pages/resources/resource_page.h"
#include "common/utils/downloader.h"
#include "common/utils/path_utils.h"
#include "gui/utils/config_manager.h"
#include "gui/utils/download_worker.h"
#include "gui/utils/i18n_cache.h"
#include "cli/runtime/systemd_client.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(tr("Vinput Configuration"));

  // Start background fetch of i18n map.
  vinput::gui::I18nCache::Get().Initialize(vinput::gui::ConfigManager::Get().Load());

  auto *centralWidget = new QWidget(this);
  setCentralWidget(centralWidget);
  auto *mainLayout = new QVBoxLayout(centralWidget);

  tabWidget_ = new QTabWidget(this);
  mainLayout->addWidget(tabWidget_);

  controlPage_ = new vinput::gui::ControlPage(this);
  resourcePage_ = new vinput::gui::ResourcePage(this);
  llmPage_ = new vinput::gui::LlmPage(this);
  hotwordPage_ = new vinput::gui::HotwordPage(this);

  tabWidget_->addTab(controlPage_, tr("Control"));
  tabWidget_->addTab(resourcePage_, tr("Resources"));
  tabWidget_->addTab(llmPage_, tr("LLM"));
  tabWidget_->addTab(hotwordPage_, tr("Hotwords"));

  // Cross-page refresh: any config change reloads affected pages.
  connect(controlPage_, &vinput::gui::ControlPage::configChanged, this,
          &MainWindow::reloadAll);
  connect(resourcePage_, &vinput::gui::ResourcePage::configChanged, this,
          &MainWindow::reloadAll);
  connect(llmPage_, &vinput::gui::LlmPage::configChanged, this,
          &MainWindow::reloadAll);

  // Bottom bar
  auto *bottomLayout = new QHBoxLayout();
  auto *btnOpenConfig = new QPushButton(tr("Open Config"), this);
  connect(btnOpenConfig, &QPushButton::clicked, this,
          &MainWindow::onOpenConfigClicked);

  auto *btnSave = new QPushButton(tr("Save Settings"), this);
  connect(btnSave, &QPushButton::clicked, this, &MainWindow::onSaveClicked);

  bottomLayout->addWidget(btnOpenConfig);
  bottomLayout->addStretch();
  bottomLayout->addWidget(btnSave);
  mainLayout->addLayout(bottomLayout);

  // Initial load
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
  QString device = controlPage_->currentDevice();
  if (!device.isEmpty() && device != "default") {
    config.global.captureDevice = device.toStdString();
  } else {
    config.global.captureDevice.clear();
  }

  // Save audio processing settings
  config.asr.normalizeAudio = controlPage_->normalizeAudio();
  config.asr.inputGain = controlPage_->inputGain();
  config.asr.vad.enabled = controlPage_->vadEnabled();
  config.global.duckOutputWhileRecording = controlPage_->duckOutputEnabled();
  config.global.duckOutputVolume = controlPage_->duckOutputVolume();

  // Save hotwords
  QString hotwordsFile = hotwordPage_->hotwordsFilePath();
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
    QFile f(hotwordsFile);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
      QTextStream(&f) << hotwordPage_->hotwordsContent();
    }
  }

  if (!vinput::gui::ConfigManager::Get().Save(config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }

  const auto result = vinput::cli::SystemctlRestartWithDiagnostics();
  if (!result.ok()) {
    vinput::cli::NotifyDaemonNotification(result.notification);
    QMessageBox::critical(this, tr("Error"),
                          QString::fromStdString(result.failure_message));
    return;
  }
  QMessageBox::information(this, tr("Success"),
                           tr("Settings saved successfully!"));
  close();
}

void MainWindow::onOpenConfigClicked() {
  const auto configPath = vinput::path::CoreConfigPath();
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QString::fromStdString(configPath.string())));
}

namespace {
std::string NormalizeNotificationLocale(std::string locale) {
  const auto colon = locale.find(':');
  if (colon != std::string::npos) {
    locale = locale.substr(0, colon);
  }
  const auto dot = locale.find('.');
  if (dot != std::string::npos) {
    locale = locale.substr(0, dot);
  }
  const auto at = locale.find('@');
  if (at != std::string::npos) {
    locale = locale.substr(0, at);
  }
  for (char &ch : locale) {
    if (ch == '-') {
      ch = '_';
    }
  }
  if (locale.empty() || locale == "C" || locale == "POSIX") {
    return {};
  }
  return locale;
}

QString localizedString(const nlohmann::json &obj) {
  if (obj.is_string()) {
    return QString::fromStdString(obj.get<std::string>());
  }
  if (!obj.is_object()) {
    return {};
  }
  const std::string locale =
      NormalizeNotificationLocale(QLocale::system().name().toStdString());
  if (obj.contains(locale)) {
    return QString::fromStdString(obj[locale].get<std::string>());
  }
  const auto underscore = locale.find('_');
  if (underscore != std::string::npos) {
    const std::string lang = locale.substr(0, underscore);
    if (obj.contains(lang)) {
      return QString::fromStdString(obj[lang].get<std::string>());
    }
  }
  if (obj.contains("en")) {
    return QString::fromStdString(obj["en"].get<std::string>());
  }
  if (!obj.empty()) {
    return QString::fromStdString(obj.begin().value().get<std::string>());
  }
  return {};
}
} // namespace

void MainWindow::checkNotification() {
  auto *worker = new vinput::gui::DownloadWorker(this);
  worker->SetTask([this](std::string * /*error*/) -> bool {
    const std::vector<std::string> urls = {
        "https://raw.githubusercontent.com/xifan2333/fcitx5-vinput/main/"
        "notification.json"};
    std::string content;
    vinput::download::Options opts;
    opts.timeout_seconds = 5;
    if (!vinput::download::DownloadText(urls, opts, &content)) {
      return true; // silent fail
    }

    nlohmann::json j;
    try {
      j = nlohmann::json::parse(content);
    } catch (...) {
      return true;
    }

    const int remote_id = j.value("id", 0);
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

    const QString title = localizedString(j.value("title", nlohmann::json{}));
    const QString text = localizedString(j.value("text", nlohmann::json{}));
    const QString url = QString::fromStdString(j.value("url", std::string{}));
    const QString id = QString::number(remote_id);

    QMetaObject::invokeMethod(
        this, [this, id, title, text, url]() {
          onNotificationReady(id, title, text, url);
        },
        Qt::QueuedConnection);

    return true;
  });
  connect(worker, &QThread::finished, worker, &QObject::deleteLater);
  worker->start();
}

void MainWindow::onNotificationReady(QString id, QString title, QString text,
                                     QString url) {
  QMessageBox msgBox(this);
  msgBox.setWindowTitle(title);
  msgBox.setText(text);
  msgBox.setIcon(QMessageBox::Information);
  auto *detailBtn =
      url.isEmpty() ? nullptr : msgBox.addButton(tr("Details"), QMessageBox::ActionRole);
  msgBox.addButton(QMessageBox::Ok);
  msgBox.exec();

  if (detailBtn && msgBox.clickedButton() == detailBtn) {
    QDesktopServices::openUrl(QUrl(url));
  }

  // Mark as read
  const auto read_path = vinput::path::ReadNotificationsPath();
  std::error_code ec;
  std::filesystem::create_directories(read_path.parent_path(), ec);
  std::ofstream ofs(read_path, std::ios::trunc);
  if (ofs) {
    ofs << id.toStdString();
  }
}
