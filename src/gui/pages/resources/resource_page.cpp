#include "pages/resources/resource_page.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "utils/gui_helpers.h"
#include "gui/utils/config_manager.h"
#include "gui/utils/i18n_cache.h"
#include "gui/utils/download_worker.h"
#include "cli/runtime/systemd_client.h"
#include "common/utils/download_progress.h"
#include "common/utils/string_utils.h"

namespace vinput::gui {

namespace {

void SetupTable(QTableWidget *t, const QStringList &headers) {
  t->setColumnCount(headers.size());
  t->setHorizontalHeaderLabels(headers);
  t->setSelectionBehavior(QAbstractItemView::SelectRows);
  t->setSelectionMode(QAbstractItemView::SingleSelection);
  t->setEditTriggers(QAbstractItemView::NoEditTriggers);
  t->setAlternatingRowColors(true);
  t->verticalHeader()->hide();
  t->horizontalHeader()->setStretchLastSection(true);
  t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
}

}  // namespace

ResourcePage::ResourcePage(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);

  auto *lblLocal = new QLabel(tr("<b>Installed Models</b>"));
  layout->addWidget(lblLocal);

  auto *topLayout = new QHBoxLayout();
  tableInstalledModels_ = new QTableWidget();
  SetupTable(tableInstalledModels_,
             {tr("Name"), tr("Type"), tr("Language"), tr("Size"),
              tr("Hotwords"), tr("Status")});
  topLayout->addWidget(tableInstalledModels_, 1);

  auto *btnLayout = new QVBoxLayout();
  btnUseModel_ = new QPushButton(tr("Use"));
  btnRemoveModel_ = new QPushButton(tr("Remove"));
  btnRefreshResources_ = new QPushButton(tr("Refresh"));
  btnLayout->addWidget(btnUseModel_);
  btnLayout->addWidget(btnRemoveModel_);
  btnLayout->addWidget(btnRefreshResources_);
  btnLayout->addStretch();
  topLayout->addLayout(btnLayout);
  layout->addLayout(topLayout);

  auto *lblRemote = new QLabel(tr("<b>Available Models</b>"));
  layout->addWidget(lblRemote);

  auto *remoteLayout = new QHBoxLayout();
  tableAvailableModels_ = new QTableWidget();
  SetupTable(tableAvailableModels_,
             {tr("Title"), tr("Description"), tr("Type"), tr("Language"),
              tr("Size"), tr("Hotwords"), tr("Status")});
  remoteLayout->addWidget(tableAvailableModels_, 1);

  btnDownloadModel_ = new QPushButton(tr("Download"));
  auto *dlLayout = new QVBoxLayout();
  dlLayout->addWidget(btnDownloadModel_);
  dlLayout->addStretch();
  remoteLayout->addLayout(dlLayout);
  layout->addLayout(remoteLayout);

  auto *lblProviders = new QLabel(tr("<b>Available ASR Providers</b>"));
  layout->addWidget(lblProviders);

  auto *providerLayout = new QHBoxLayout();
  tableAvailableProviders_ = new QTableWidget();
  SetupTable(tableAvailableProviders_,
             {tr("Title"), tr("Description"), tr("Mode"), tr("Status")});
  providerLayout->addWidget(tableAvailableProviders_, 1);

  btnAddProvider_ = new QPushButton(tr("Install"));
  auto *providerBtnLayout = new QVBoxLayout();
  providerBtnLayout->addWidget(btnAddProvider_);
  providerBtnLayout->addStretch();
  providerLayout->addLayout(providerBtnLayout);
  layout->addLayout(providerLayout);

  auto *lblAdapters = new QLabel(tr("<b>Available LLM Adapters</b>"));
  layout->addWidget(lblAdapters);

  auto *adapterLayout = new QHBoxLayout();
  tableAvailableAdapters_ = new QTableWidget();
  SetupTable(tableAvailableAdapters_,
             {tr("Title"), tr("Description"), tr("Status")});
  adapterLayout->addWidget(tableAvailableAdapters_, 1);

  btnAddAdapter_ = new QPushButton(tr("Install"));
  auto *adapterBtnLayout = new QVBoxLayout();
  adapterBtnLayout->addWidget(btnAddAdapter_);
  adapterBtnLayout->addStretch();
  adapterLayout->addLayout(adapterBtnLayout);
  layout->addLayout(adapterLayout);

  textLog_ = new QTextEdit();
  textLog_->setReadOnly(true);
  textLog_->setMaximumHeight(100);

  downloadStatusLabel_ = new QLabel();
  downloadStatusLabel_->setVisible(false);
  layout->addWidget(downloadStatusLabel_);

  downloadProgressBar_ = new QProgressBar();
  downloadProgressBar_->setRange(0, 100);
  downloadProgressBar_->setValue(0);
  downloadProgressBar_->setVisible(false);
  layout->addWidget(downloadProgressBar_);

  layout->addWidget(textLog_);

  connect(btnUseModel_, &QPushButton::clicked, this,
          &ResourcePage::onUseModelClicked);
  connect(btnRemoveModel_, &QPushButton::clicked, this,
          &ResourcePage::onRemoveModelClicked);
  connect(btnRefreshResources_, &QPushButton::clicked, this,
          &ResourcePage::refreshAll);
  connect(btnDownloadModel_, &QPushButton::clicked, this,
          &ResourcePage::onDownloadModelClicked);
  connect(btnAddProvider_, &QPushButton::clicked, this,
          &ResourcePage::onAddProviderClicked);
  connect(btnAddAdapter_, &QPushButton::clicked, this,
          &ResourcePage::onAddAdapterClicked);

  connect(&I18nCache::Get(), &I18nCache::mapUpdated, this,
          &ResourcePage::refreshAll);

  QTimer::singleShot(0, this, &ResourcePage::refreshAll);
}

void ResourcePage::reload() { refreshAll(); }

// ---------------------------------------------------------------------------
// Populate helpers
// ---------------------------------------------------------------------------

void ResourcePage::populateLocalModels(const std::vector<ModelSummary> &models) {
  const QPalette pal = QApplication::palette();
  const QColor colorPositive = pal.color(QPalette::Active, QPalette::Link);
  const QColor colorDisabled = pal.color(QPalette::Disabled, QPalette::Text);
  const QColor colorHighlight = pal.color(QPalette::Active, QPalette::Highlight);
  const QColor colorError = QColor(198, 40, 40);

  tableInstalledModels_->setRowCount(0);

  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto &model : models) {
    QString id = QString::fromStdString(model.id);
    QString titleStr = QString::fromStdString(vinput::registry::LookupI18n(i18n_map, model.id + ".title", ""));
    QString title = titleStr.isEmpty() ? id : titleStr;

    int row = tableInstalledModels_->rowCount();
    tableInstalledModels_->insertRow(row);
    tableInstalledModels_->setItem(row, 0, MakeCell(title, id));
    tableInstalledModels_->setItem(row, 1, MakeCell(QString::fromStdString(model.model_type)));
    tableInstalledModels_->setItem(row, 2, MakeCell(QString::fromStdString(model.language)));
    auto *sizeCell = MakeCell(QString::fromStdString(vinput::str::FormatSize(model.size_bytes)));
    sizeCell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tableInstalledModels_->setItem(row, 3, sizeCell);

    auto *hwCell = MakeCell(model.supports_hotwords ? tr("yes") : tr("no"));
    hwCell->setForeground(model.supports_hotwords ? colorPositive : colorDisabled);
    tableInstalledModels_->setItem(row, 4, hwCell);

    QString status;
    if (model.state == ModelState::Active) status = tr("active");
    else if (model.state == ModelState::Broken) status = tr("broken");
    else status = tr("installed");

    auto *stCell = MakeCell(status);
    if (model.state == ModelState::Active) {
      stCell->setForeground(colorHighlight);
      QFont f = stCell->font();
      f.setBold(true);
      stCell->setFont(f);
    } else if (model.state == ModelState::Broken) {
      stCell->setForeground(colorError);
    }
    tableInstalledModels_->setItem(row, 5, stCell);
  }
}

void ResourcePage::populateRemoteModels(const std::vector<RemoteModelEntry> &models) {
  const QPalette pal = QApplication::palette();
  const QColor colorPositive = pal.color(QPalette::Active, QPalette::Link);
  const QColor colorDisabled = pal.color(QPalette::Disabled, QPalette::Text);

  tableAvailableModels_->setRowCount(0);
  
  CoreConfig config = ConfigManager::Get().Load();
  ModelManager manager(ResolveModelBaseDir(config).string());
  auto localModels = manager.ListDetailed("");
  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto &model : models) {
    QString id = QString::fromStdString(model.id);

    QString titleStr = QString::fromStdString(vinput::registry::LookupI18n(i18n_map, model.id + ".title", ""));
    QString title = titleStr.isEmpty() ? id : titleStr;
    QString desc = QString::fromStdString(vinput::registry::LookupI18n(i18n_map, model.id + ".description", ""));

    int row = tableAvailableModels_->rowCount();
    tableAvailableModels_->insertRow(row);
    tableAvailableModels_->setItem(row, 0, MakeCell(title, id));
    tableAvailableModels_->setItem(row, 1, MakeCell(desc));
    tableAvailableModels_->setItem(row, 2, MakeCell(QString::fromStdString(model.model_type())));
    tableAvailableModels_->setItem(row, 3, MakeCell(QString::fromStdString(model.language)));
    auto *sizeCell = MakeCell(QString::fromStdString(vinput::str::FormatSize(model.size_bytes)));
    sizeCell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tableAvailableModels_->setItem(row, 4, sizeCell);

    auto *hwCell = MakeCell(model.supports_hotwords() ? tr("yes") : tr("no"));
    hwCell->setForeground(model.supports_hotwords() ? colorPositive : colorDisabled);
    tableAvailableModels_->setItem(row, 5, hwCell);

    bool installed = std::any_of(localModels.begin(), localModels.end(), [&](const ModelSummary& m){ return m.id == model.id; });
    QString status = installed ? tr("installed") : tr("available");

    auto *stCell = MakeCell(status);
    if (installed) {
      stCell->setForeground(colorDisabled);
      for (int c = 0; c < tableAvailableModels_->columnCount(); ++c) {
        if (auto *ci = tableAvailableModels_->item(row, c))
          ci->setFlags(ci->flags() & ~Qt::ItemIsEnabled);
      }
    } else {
      stCell->setForeground(colorPositive);
    }
    tableAvailableModels_->setItem(row, 6, stCell);
  }
}

void ResourcePage::populateRemoteProviders(const std::vector<vinput::script::RegistryEntry> &providers) {
  const QPalette pal = QApplication::palette();
  const QColor colorPositive = pal.color(QPalette::Active, QPalette::Link);
  const QColor colorDisabled = pal.color(QPalette::Disabled, QPalette::Text);

  tableAvailableProviders_->setRowCount(0);
  CoreConfig config = ConfigManager::Get().Load();
  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto &entry : providers) {
    QString id = QString::fromStdString(entry.id);

    QString titleStr = QString::fromStdString(vinput::registry::LookupI18n(i18n_map, entry.id + ".title", ""));
    QString title = titleStr.isEmpty() ? id : titleStr;
    QString desc = QString::fromStdString(vinput::registry::LookupI18n(i18n_map, entry.id + ".description", ""));

    int row = tableAvailableProviders_->rowCount();
    tableAvailableProviders_->insertRow(row);
    tableAvailableProviders_->setItem(row, 0, MakeCell(title, id));
    tableAvailableProviders_->setItem(row, 1, MakeCell(desc));
    tableAvailableProviders_->setItem(row, 2, MakeCell(entry.stream ? tr("stream") : tr("non-stream")));

    bool installed = ResolveAsrProvider(config, entry.id) != nullptr;
    QString status = installed ? tr("installed") : tr("available");

    auto *stCell = MakeCell(status);
    if (installed) {
      stCell->setForeground(colorDisabled);
      for (int c = 0; c < tableAvailableProviders_->columnCount(); ++c) {
        if (auto *cell = tableAvailableProviders_->item(row, c))
          cell->setFlags(cell->flags() & ~Qt::ItemIsEnabled);
      }
    } else {
      stCell->setForeground(colorPositive);
    }
    tableAvailableProviders_->setItem(row, 3, stCell);
  }
}

void ResourcePage::populateRemoteAdapters(const std::vector<vinput::script::RegistryEntry> &adapters) {
  const QPalette pal = QApplication::palette();
  const QColor colorPositive = pal.color(QPalette::Active, QPalette::Link);
  const QColor colorDisabled = pal.color(QPalette::Disabled, QPalette::Text);

  tableAvailableAdapters_->setRowCount(0);
  CoreConfig config = ConfigManager::Get().Load();
  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto &entry : adapters) {
    QString id = QString::fromStdString(entry.id);

    QString titleStr = QString::fromStdString(vinput::registry::LookupI18n(i18n_map, entry.id + ".title", ""));
    QString title = titleStr.isEmpty() ? id : titleStr;
    QString desc = QString::fromStdString(vinput::registry::LookupI18n(i18n_map, entry.id + ".description", ""));

    int row = tableAvailableAdapters_->rowCount();
    tableAvailableAdapters_->insertRow(row);
    tableAvailableAdapters_->setItem(row, 0, MakeCell(title, id));
    tableAvailableAdapters_->setItem(row, 1, MakeCell(desc));

    bool installed = ResolveLlmAdapter(config, entry.id) != nullptr;
    QString status = installed ? tr("installed") : tr("available");

    auto *stCell = MakeCell(status);
    if (installed) {
      stCell->setForeground(colorDisabled);
      for (int c = 0; c < tableAvailableAdapters_->columnCount(); ++c) {
        if (auto *cell = tableAvailableAdapters_->item(row, c))
          cell->setFlags(cell->flags() & ~Qt::ItemIsEnabled);
      }
    } else {
      stCell->setForeground(colorPositive);
    }
    tableAvailableAdapters_->setItem(row, 2, stCell);
  }
}

void ResourcePage::refreshAll() {
  CoreConfig config = ConfigManager::Get().Load();
  QString baseDir = QString::fromStdString(ResolveModelBaseDir(config).string());
  QString preferredModel = QString::fromStdString(ResolvePreferredLocalModel(config));

  ModelManager manager(baseDir.toStdString());
  auto localModels = manager.ListDetailed(preferredModel.toStdString());
  populateLocalModels(localModels);

  btnRefreshResources_->setEnabled(false);
  textLog_->append(tr("Fetching remote registry..."));

  QtConcurrent::run([this, config, baseDir]() {
     ModelRepository repo(baseDir.toStdString());
     std::string err;
     
     auto registryUrls = ResolveModelRegistryUrls(config);
     auto remoteModels = repo.FetchRegistry(config, registryUrls, &err);
     if (!err.empty()) {
         QMetaObject::invokeMethod(this, [this, err]() { textLog_->append(tr("Models fetch error: %1").arg(QString::fromStdString(err))); });
     }
     
     err.clear();
     auto providerUrls = ResolveAsrProviderRegistryUrls(config);
     auto remoteProviders = vinput::script::FetchRegistry(config, vinput::script::Kind::kAsrProvider, providerUrls, &err);
     if (!err.empty()) {
         QMetaObject::invokeMethod(this, [this, err]() { textLog_->append(tr("Providers fetch error: %1").arg(QString::fromStdString(err))); });
     }
     
     err.clear();
     auto adapterUrls = ResolveLlmAdapterRegistryUrls(config);
     auto remoteAdapters = vinput::script::FetchRegistry(config, vinput::script::Kind::kLlmAdapter, adapterUrls, &err);
     if (!err.empty()) {
         QMetaObject::invokeMethod(this, [this, err]() { textLog_->append(tr("Adapters fetch error: %1").arg(QString::fromStdString(err))); });
     }

     QMetaObject::invokeMethod(this, [this, remoteModels, remoteProviders, remoteAdapters]() {
         populateRemoteModels(remoteModels);
         populateRemoteProviders(remoteProviders);
         populateRemoteAdapters(remoteAdapters);
         btnRefreshResources_->setEnabled(true);
         textLog_->append(tr("Registry fetch completed."));
     });
  });
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void ResourcePage::abortDownload() {
  if (downloadWorker_ && downloadWorker_->isRunning()) {
      downloadWorker_->disconnect();
      downloadWorker_->quit();
      downloadWorker_->wait();
      delete downloadWorker_;
      downloadWorker_ = nullptr;
  }
}

void ResourcePage::onUseModelClicked() {
  auto items = tableInstalledModels_->selectedItems();
  if (items.isEmpty()) return;
  QString model_name = tableInstalledModels_->item(tableInstalledModels_->currentRow(), 0)->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  std::string err;
  if (!SetPreferredLocalModel(&config, model_name.toStdString(), &err)) {
      QMessageBox::critical(this, tr("Error"), QString::fromStdString(err));
      return;
  }
  if (!ConfigManager::Get().Save(config)) {
      QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
      return;
  }

  vinput::cli::SystemctlRestart();
  QMessageBox::information(
      this, tr("Local Model Updated"),
      tr("Selected model '%1' has been assigned to the preferred local ASR provider.").arg(model_name));
  refreshAll();
  emit configChanged();
}

void ResourcePage::onRemoveModelClicked() {
  auto items = tableInstalledModels_->selectedItems();
  if (items.isEmpty()) return;
  QString model_name = tableInstalledModels_->item(tableInstalledModels_->currentRow(), 0)->data(Qt::UserRole).toString();

  auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove model '%1'?").arg(model_name));
  if (response == QMessageBox::Yes) {
    CoreConfig config = ConfigManager::Get().Load();
    ModelManager manager(ResolveModelBaseDir(config).string());
    std::string err;
    if (!manager.Remove(model_name.toStdString(), &err)) {
        QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
        return;
    }
    
    // Check if was preferred
    if (ResolvePreferredLocalModel(config) == model_name.toStdString()) {
        SetPreferredLocalModel(&config, "", &err);
        ConfigManager::Get().Save(config);
    }
    textLog_->append(tr("Removed %1.").arg(model_name));
    refreshAll();
    emit configChanged();
  }
}

void ResourcePage::onDownloadProgress(int percent, QString speed) {
    downloadStatusLabel_->setText(tr("Downloading... %1% at %2").arg(percent).arg(speed));
    downloadStatusLabel_->setVisible(true);
    downloadProgressBar_->setValue(percent);
    downloadProgressBar_->setVisible(true);
}

void ResourcePage::onDownloadError(QString msg) {
    QMessageBox::critical(this, tr("Download Error"), msg);
    onDownloadFinished();
}

void ResourcePage::onDownloadFinished() {
    btnDownloadModel_->setEnabled(true);
    btnAddProvider_->setEnabled(true);
    btnAddAdapter_->setEnabled(true);
    btnRemoveModel_->setEnabled(true);
    downloadStatusLabel_->clear();
    downloadStatusLabel_->setVisible(false);
    downloadProgressBar_->setValue(0);
    downloadProgressBar_->setVisible(false);
    if (downloadWorker_) {
        downloadWorker_->deleteLater();
        downloadWorker_ = nullptr;
    }
    refreshAll();
    emit configChanged();
}

void ResourcePage::onDownloadModelClicked() {
  auto items = tableAvailableModels_->selectedItems();
  if (items.isEmpty()) return;
  QString model_name = tableAvailableModels_->item(tableAvailableModels_->currentRow(), 0)->data(Qt::UserRole).toString();

  abortDownload();
  btnDownloadModel_->setEnabled(false);
  btnRemoveModel_->setEnabled(false);
  downloadStatusLabel_->setText(tr("Preparing download..."));
  downloadStatusLabel_->setVisible(true);
  downloadProgressBar_->setValue(0);
  downloadProgressBar_->setVisible(true);
  
  downloadWorker_ = new DownloadWorker(this);
  connect(downloadWorker_, &DownloadWorker::progress, this, &ResourcePage::onDownloadProgress, Qt::QueuedConnection);
  connect(downloadWorker_, &DownloadWorker::error, this, &ResourcePage::onDownloadError);
  connect(downloadWorker_, &QThread::finished, this, &ResourcePage::onDownloadFinished);

  CoreConfig config = ConfigManager::Get().Load();
  
  downloadWorker_->SetTask([config, model_name, worker=downloadWorker_](std::string* err) -> bool {
      ModelRepository repo(ResolveModelBaseDir(config).string());
      auto urls = ResolveModelRegistryUrls(config);
      return repo.InstallModel(config, urls, model_name.toStdString(), [worker](const InstallProgress& p) {
         if (p.total_bytes > 0) {
             int percent = static_cast<int>((p.downloaded_bytes * 100) / p.total_bytes);
             QString speed = QString::fromStdString(vinput::str::FormatSize(static_cast<uint64_t>(p.speed_bps))) + "/s";
             worker->ReportProgress(percent, speed);
         }
      }, err);
  });
  textLog_->append(tr("Starting download for %1...").arg(model_name));
  downloadWorker_->start();
}

void ResourcePage::onAddProviderClicked() {
  auto items = tableAvailableProviders_->selectedItems();
  if (items.isEmpty()) return;
  QString id = tableAvailableProviders_->item(tableAvailableProviders_->currentRow(), 0)->data(Qt::UserRole).toString();

  abortDownload();
  btnAddProvider_->setEnabled(false);
  
  downloadWorker_ = new DownloadWorker(this);
  connect(downloadWorker_, &DownloadWorker::error, this, &ResourcePage::onDownloadError);
  connect(downloadWorker_, &QThread::finished, this, [&](){
      vinput::cli::SystemctlRestart();
      onDownloadFinished();
  });

  CoreConfig config = ConfigManager::Get().Load();
  
  downloadWorker_->SetTask([config, id](std::string* err) -> bool {
      auto urls = ResolveAsrProviderRegistryUrls(config);
      auto entries = vinput::script::FetchRegistry(config, vinput::script::Kind::kAsrProvider, urls, err);
      if (!err->empty()) return false;
      
      auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e){ return e.id == id.toStdString(); });
      if (it == entries.end()) {
          *err = "Provider not found in registry.";
          return false;
      }
      
      std::filesystem::path scriptPath;
      if (!vinput::script::DownloadScript(*it, vinput::script::Kind::kAsrProvider, &scriptPath, err)) {
          return false;
      }
      
      CoreConfig mutConfig = config;
      if (!vinput::script::MaterializeAsrProvider(&mutConfig, *it, scriptPath, err)) {
          return false;
      }
      return ConfigManager::Get().Save(mutConfig);
  });
  textLog_->append(tr("Installing provider %1...").arg(id));
  downloadWorker_->start();
}

void ResourcePage::onAddAdapterClicked() {
  auto items = tableAvailableAdapters_->selectedItems();
  if (items.isEmpty()) return;
  QString id = tableAvailableAdapters_->item(tableAvailableAdapters_->currentRow(), 0)->data(Qt::UserRole).toString();

  abortDownload();
  btnAddAdapter_->setEnabled(false);
  
  downloadWorker_ = new DownloadWorker(this);
  connect(downloadWorker_, &DownloadWorker::error, this, &ResourcePage::onDownloadError);
  connect(downloadWorker_, &QThread::finished, this, &ResourcePage::onDownloadFinished);

  CoreConfig config = ConfigManager::Get().Load();
  
  downloadWorker_->SetTask([config, id](std::string* err) -> bool {
      auto urls = ResolveLlmAdapterRegistryUrls(config);
      auto entries = vinput::script::FetchRegistry(config, vinput::script::Kind::kLlmAdapter, urls, err);
      if (!err->empty()) return false;
      
      auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e){ return e.id == id.toStdString(); });
      if (it == entries.end()) {
          *err = "Adapter not found in registry.";
          return false;
      }
      
      std::filesystem::path scriptPath;
      if (!vinput::script::DownloadScript(*it, vinput::script::Kind::kLlmAdapter, &scriptPath, err)) {
          return false;
      }
      
      CoreConfig mutConfig = config;
      if (!vinput::script::MaterializeLlmAdapter(&mutConfig, *it, scriptPath, err)) {
          return false;
      }
      return ConfigManager::Get().Save(mutConfig);
  });
  textLog_->append(tr("Installing adapter %1...").arg(id));
  downloadWorker_->start();
}

}  // namespace vinput::gui
