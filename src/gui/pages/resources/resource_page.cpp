#include "pages/resources/resource_page.h"

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtCore/qabstractitemmodel.h>
#include <QtCore/qcoreevent.h>
#include <QtCore/qnamespace.h>
#include <QtCore/qstringliteral.h>
#include <QtCore/qurl.h>
#include <QtGui/qdesktopservices.h>
#include <QtGui/qevent.h>
#include <QtGui/qpainter.h>
#include <QtWidgets/qstyle.h>
#include <QtWidgets/qstyleditemdelegate.h>
#include <QtWidgets/qstyleoption.h>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <variant>

#include "common/config/core_config_types.h"
#include "common/llm/adapter_manager.h"
#include "common/registry/registry_models.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/string_utils.h"

#include "cli/runtime/dbus_client.h"

#include "gui/utils/config_manager.h"
#include "gui/utils/download_worker.h"
#include "gui/utils/i18n_cache.h"

#include "config.h"
#include "utils/gui_helpers.h"

namespace vinput::gui {

namespace {

class LinkDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    if (!index.data(Qt::UserRole).toString().isEmpty()) {
      opt.font.setUnderline(true);
      if ((opt.state & QStyle::State_Selected) == 0) {
        opt.palette.setColor(QPalette::Text, opt.palette.link().color());
      }
    }
    QStyledItemDelegate::paint(painter, opt, index);
  }

  bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override {
    if (event != nullptr && event->type() == QEvent::MouseButtonRelease) {
      const auto* mouseEvent = dynamic_cast<const QMouseEvent*>(event);
      if (mouseEvent != nullptr && mouseEvent->button() == Qt::LeftButton) {
        const QString url = index.data(Qt::UserRole).toString();
        if (!url.isEmpty()) {
          QDesktopServices::openUrl(QUrl(url));
          return true;
        }
      }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
  }
};

void SetupTable(QTableWidget* t, const QStringList& headers) {
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

QLineEdit* CreateFilterEdit(const QString& placeholder, QWidget* parent) {
  auto* edit = new QLineEdit(parent);
  edit->setClearButtonEnabled(true);
  edit->setPlaceholderText(placeholder);
  return edit;
}

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

template <typename Callback> void RunReloadAsrBackendAsync(ResourcePage* page, Callback callback) {
  QPointer<ResourcePage> self(page);
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

} // namespace

ResourcePage::ResourcePage(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  resourceTabs_ = new QTabWidget(this);
  layout->addWidget(resourceTabs_, 1);

#if VINPUT_ENABLE_LOCAL_ASR
  auto* modelsTab = new QWidget(this);
  auto* modelsLayout = new QVBoxLayout(modelsTab);

  auto* lblLocal = new QLabel(tr("<b>Installed Models</b>"));
  modelsLayout->addWidget(lblLocal);
  filterInstalledModels_ = CreateFilterEdit(tr("Filter installed models..."), modelsTab);
  modelsLayout->addWidget(filterInstalledModels_);

  auto* topLayout = new QHBoxLayout();
  tableInstalledModels_ = new QTableWidget();
  SetupTable(tableInstalledModels_,
             {tr("Name"), tr("Type"), tr("Language"), tr("Size"), tr("Hotwords"), tr("Status")});
  topLayout->addWidget(tableInstalledModels_, 1);

  auto* btnLayout = new QVBoxLayout();
  btnUseModel_ = new QPushButton(tr("Use"));
  btnRemoveModel_ = new QPushButton(tr("Remove"));
  btnRefreshResources_ = new QPushButton(tr("Refresh"));
  btnLayout->addWidget(btnUseModel_);
  btnLayout->addWidget(btnRemoveModel_);
  btnLayout->addWidget(btnRefreshResources_);
  btnLayout->addStretch();
  topLayout->addLayout(btnLayout);
  modelsLayout->addLayout(topLayout, 1);

  auto* lblRemote = new QLabel(tr("<b>Available Models</b>"));
  modelsLayout->addWidget(lblRemote);
  filterAvailableModels_ = CreateFilterEdit(tr("Filter available models..."), modelsTab);
  modelsLayout->addWidget(filterAvailableModels_);

  auto* remoteLayout = new QHBoxLayout();
  tableAvailableModels_ = new QTableWidget();
  SetupTable(tableAvailableModels_, {tr("Title"), tr("Description"), tr("Type"), tr("Language"),
                                     tr("Size"), tr("Hotwords"), tr("Status")});
  remoteLayout->addWidget(tableAvailableModels_, 1);

  btnDownloadModel_ = new QPushButton(tr("Download"));
  auto* dlLayout = new QVBoxLayout();
  dlLayout->addWidget(btnDownloadModel_);
  dlLayout->addStretch();
  remoteLayout->addLayout(dlLayout);
  modelsLayout->addLayout(remoteLayout, 1);
  resourceTabs_->addTab(modelsTab, tr("Models"));
#else
  filterInstalledModels_ = nullptr;
  filterAvailableModels_ = nullptr;
  tableInstalledModels_ = nullptr;
  tableAvailableModels_ = nullptr;
  btnUseModel_ = nullptr;
  btnRemoveModel_ = nullptr;
  btnDownloadModel_ = nullptr;
  btnRefreshResources_ = new QPushButton(tr("Refresh"));
#endif

  auto* providersTab = new QWidget(this);
  auto* providersLayout = new QVBoxLayout(providersTab);
  auto* lblProviders = new QLabel(tr("<b>Available ASR Providers</b>"));
  providersLayout->addWidget(lblProviders);
  filterAvailableProviders_ =
      CreateFilterEdit(tr("Filter available ASR providers..."), providersTab);
  providersLayout->addWidget(filterAvailableProviders_);

  auto* providerLayout = new QHBoxLayout();
  tableAvailableProviders_ = new QTableWidget();
  SetupTable(tableAvailableProviders_,
             {tr("Title"), tr("Description"), tr("Mode"), tr("Status"), tr("README")});
  tableAvailableProviders_->setItemDelegateForColumn(4, new LinkDelegate(tableAvailableProviders_));
  providerLayout->addWidget(tableAvailableProviders_, 1);

  btnAddProvider_ = new QPushButton(tr("Install"));
  btnRemoveProvider_ = new QPushButton(tr("Remove"));
  auto* providerBtnLayout = new QVBoxLayout();
  providerBtnLayout->addWidget(btnAddProvider_);
  providerBtnLayout->addWidget(btnRemoveProvider_);
#if !VINPUT_ENABLE_LOCAL_ASR
  providerBtnLayout->addWidget(btnRefreshResources_);
#endif
  providerBtnLayout->addStretch();
  providerLayout->addLayout(providerBtnLayout);
  providersLayout->addLayout(providerLayout, 1);
  resourceTabs_->addTab(providersTab, tr("ASR Providers"));

  auto* adaptersTab = new QWidget(this);
  auto* adaptersLayout = new QVBoxLayout(adaptersTab);
  auto* lblAdapters = new QLabel(tr("<b>Available LLM Adapters</b>"));
  adaptersLayout->addWidget(lblAdapters);
  filterAvailableAdapters_ = CreateFilterEdit(tr("Filter available LLM adapters..."), adaptersTab);
  adaptersLayout->addWidget(filterAvailableAdapters_);

  auto* adapterLayout = new QHBoxLayout();
  tableAvailableAdapters_ = new QTableWidget();
  SetupTable(tableAvailableAdapters_, {tr("Title"), tr("Description"), tr("Status"), tr("README")});
  tableAvailableAdapters_->setItemDelegateForColumn(3, new LinkDelegate(tableAvailableAdapters_));
  adapterLayout->addWidget(tableAvailableAdapters_, 1);

  btnAddAdapter_ = new QPushButton(tr("Install"));
  btnRemoveAdapter_ = new QPushButton(tr("Remove"));
  auto* adapterBtnLayout = new QVBoxLayout();
  adapterBtnLayout->addWidget(btnAddAdapter_);
  adapterBtnLayout->addWidget(btnRemoveAdapter_);
  adapterBtnLayout->addStretch();
  adapterLayout->addLayout(adapterBtnLayout);
  adaptersLayout->addLayout(adapterLayout, 1);
  resourceTabs_->addTab(adaptersTab, tr("LLM Adapters"));

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

#if VINPUT_ENABLE_LOCAL_ASR
  connect(btnUseModel_, &QPushButton::clicked, this, &ResourcePage::onUseModelClicked);
  connect(btnRemoveModel_, &QPushButton::clicked, this, &ResourcePage::onRemoveModelClicked);
  connect(btnDownloadModel_, &QPushButton::clicked, this, &ResourcePage::onDownloadModelClicked);
  connect(filterInstalledModels_, &QLineEdit::textChanged, this,
          [this](const QString& text) { applyTableFilter(tableInstalledModels_, text); });
  connect(filterAvailableModels_, &QLineEdit::textChanged, this,
          [this](const QString& text) { applyTableFilter(tableAvailableModels_, text); });
#endif
  connect(btnRefreshResources_, &QPushButton::clicked, this, &ResourcePage::refreshAll);
  connect(btnAddProvider_, &QPushButton::clicked, this, &ResourcePage::onAddProviderClicked);
  connect(btnRemoveProvider_, &QPushButton::clicked, this, &ResourcePage::onRemoveProviderClicked);
  connect(btnAddAdapter_, &QPushButton::clicked, this, &ResourcePage::onAddAdapterClicked);
  connect(btnRemoveAdapter_, &QPushButton::clicked, this, &ResourcePage::onRemoveAdapterClicked);
  connect(tableAvailableProviders_, &QTableWidget::itemSelectionChanged, this,
          &ResourcePage::updateProviderButtons);
  connect(tableAvailableAdapters_, &QTableWidget::itemSelectionChanged, this,
          &ResourcePage::updateAdapterButtons);
  connect(tableAvailableProviders_, &QTableWidget::cellDoubleClicked, this,
          [this](int row, int col) {
            if (col == 4) {
              const auto* item = tableAvailableProviders_->item(row, col);
              if (item != nullptr) {
                const QString url = item->data(Qt::UserRole).toString();
                if (!url.isEmpty()) {
                  QDesktopServices::openUrl(QUrl(url));
                }
              }
            }
          });
  connect(tableAvailableAdapters_, &QTableWidget::cellDoubleClicked, this,
          [this](int row, int col) {
            if (col == 3) {
              const auto* item = tableAvailableAdapters_->item(row, col);
              if (item != nullptr) {
                const QString url = item->data(Qt::UserRole).toString();
                if (!url.isEmpty()) {
                  QDesktopServices::openUrl(QUrl(url));
                }
              }
            }
          });
  connect(filterAvailableProviders_, &QLineEdit::textChanged, this,
          [this](const QString& text) { applyTableFilter(tableAvailableProviders_, text); });
  connect(filterAvailableAdapters_, &QLineEdit::textChanged, this,
          [this](const QString& text) { applyTableFilter(tableAvailableAdapters_, text); });

  btnAddProvider_->setEnabled(false);
  btnRemoveProvider_->setEnabled(false);
  btnAddAdapter_->setEnabled(false);
  btnRemoveAdapter_->setEnabled(false);

  connect(&I18nCache::Get(), &I18nCache::mapUpdated, this,
          [this](quint64 generation) { finishRefreshAfterI18n({}, generation); });
  connect(&I18nCache::Get(), &I18nCache::reloadFailed, this, &ResourcePage::finishRefreshAfterI18n);

#if VINPUT_ENABLE_LOCAL_ASR
  // Show installed models immediately; remote lists fill after refreshAll.
  {
    CoreConfig config = ConfigManager::Get().Load();
    ModelManager manager(ResolveModelBaseDir(config).string());
    populateLocalModels(manager.ListDetailed(ResolvePreferredLocalModel(config)));
  }
#endif

  QTimer::singleShot(0, this, &ResourcePage::refreshAll);
}

void ResourcePage::reload() {
  refreshAll();
}

void ResourcePage::refreshLocalizedTables() {
#if VINPUT_ENABLE_LOCAL_ASR
  CoreConfig config = ConfigManager::Get().Load();
  ModelManager manager(ResolveModelBaseDir(config).string());
  populateLocalModels(manager.ListDetailed(ResolvePreferredLocalModel(config)));
  populateRemoteModels(remoteModels_);
#endif
  populateRemoteProviders(remoteProviders_);
  populateRemoteAdapters(remoteAdapters_);
}

void ResourcePage::finishRefreshAfterI18n(const QString& error, quint64 generation) {
  // Always repaint with the latest cached map + stored remote entries so
  // startup disk preload and later mapUpdated events refresh titles.
  refreshLocalizedTables();

  // Only the i18n reload started by the current Refresh may complete the wait.
  // Startup preload or a superseded reload must not re-enable the button early.
  if (!refreshWaitingForI18n_ || generation != i18nWaitGeneration_) {
    return;
  }

  if (!error.isEmpty()) {
    textLog_->append(tr("I18n cache reload error: %1").arg(error));
  }
  refreshWaitingForI18n_ = false;
  i18nWaitGeneration_ = 0;
  btnRefreshResources_->setEnabled(true);
  textLog_->append(tr("Registry fetch completed."));
}

// ---------------------------------------------------------------------------
// Populate helpers
// ---------------------------------------------------------------------------

void ResourcePage::applyTableFilter(QTableWidget* table, const QString& filter_text) {
  if (!table) {
    return;
  }

  const QString needle = filter_text.trimmed();
  for (int row = 0; row < table->rowCount(); ++row) {
    bool matches = needle.isEmpty();
    if (!matches) {
      for (int col = 0; col < table->columnCount(); ++col) {
        const auto* item = table->item(row, col);
        if (!item) {
          continue;
        }
        const QString haystack = item->text() + '\n' + item->data(Qt::UserRole).toString();
        if (haystack.contains(needle, Qt::CaseInsensitive)) {
          matches = true;
          break;
        }
      }
    }
    table->setRowHidden(row, !matches);
  }
}

void ResourcePage::populateLocalModels(const std::vector<ModelSummary>& models) {
#if !VINPUT_ENABLE_LOCAL_ASR
  (void)models;
  return;
#else
  if (tableInstalledModels_ == nullptr) {
    return;
  }
  tableInstalledModels_->setRowCount(0);

  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto& model : models) {
    QString id = QString::fromStdString(model.id);
    QString titleStr =
        QString::fromStdString(vinput::registry::LookupI18n(i18n_map, model.id + ".title", ""));
    QString title = titleStr.isEmpty() ? id : titleStr;

    int row = tableInstalledModels_->rowCount();
    tableInstalledModels_->insertRow(row);
    tableInstalledModels_->setItem(row, 0, MakeCell(title, id));
    tableInstalledModels_->setItem(row, 1, MakeCell(QString::fromStdString(model.model_type)));
    tableInstalledModels_->setItem(row, 2, MakeCell(QString::fromStdString(model.language)));
    auto* sizeCell = MakeCell(QString::fromStdString(vinput::str::FormatSize(model.size_bytes)));
    sizeCell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tableInstalledModels_->setItem(row, 3, sizeCell);

    tableInstalledModels_->setItem(row, 4,
                                   MakeCell(model.supports_hotwords ? tr("yes") : tr("no")));

    QString status;
    if (model.state == ModelState::Active)
      status = tr("active");
    else if (model.state == ModelState::Broken)
      status = tr("broken");
    else
      status = tr("installed");

    auto* stCell = MakeCell(status);
    if (model.state == ModelState::Active) {
      QFont f = stCell->font();
      f.setBold(true);
      stCell->setFont(f);
    }
    tableInstalledModels_->setItem(row, 5, stCell);
  }

  applyTableFilter(tableInstalledModels_, filterInstalledModels_->text());
#endif
}

void ResourcePage::populateRemoteModels(const std::vector<RemoteModelEntry>& models) {
#if !VINPUT_ENABLE_LOCAL_ASR
  (void)models;
  return;
#else
  if (tableAvailableModels_ == nullptr) {
    return;
  }
  tableAvailableModels_->setRowCount(0);

  CoreConfig config = ConfigManager::Get().Load();
  ModelManager manager(ResolveModelBaseDir(config).string());
  auto localModels = manager.ListDetailed("");
  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto& model : models) {
    QString id = QString::fromStdString(model.id);

    QString titleStr =
        QString::fromStdString(vinput::registry::LookupI18n(i18n_map, model.id + ".title", ""));
    QString title = titleStr.isEmpty() ? id : titleStr;
    QString desc = QString::fromStdString(
        vinput::registry::LookupI18n(i18n_map, model.id + ".description", ""));

    int row = tableAvailableModels_->rowCount();
    tableAvailableModels_->insertRow(row);
    tableAvailableModels_->setItem(row, 0, MakeCell(title, id));
    tableAvailableModels_->setItem(row, 1, MakeCell(desc));
    tableAvailableModels_->setItem(row, 2, MakeCell(QString::fromStdString(model.model_type())));
    tableAvailableModels_->setItem(row, 3, MakeCell(QString::fromStdString(model.language)));
    auto* sizeCell = MakeCell(QString::fromStdString(vinput::str::FormatSize(model.size_bytes)));
    sizeCell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tableAvailableModels_->setItem(row, 4, sizeCell);

    tableAvailableModels_->setItem(row, 5,
                                   MakeCell(model.supports_hotwords() ? tr("yes") : tr("no")));

    bool installed = std::any_of(localModels.begin(), localModels.end(),
                                 [&](const ModelSummary& m) { return m.id == model.id; });
    QString status = installed ? tr("installed") : tr("available");

    auto* stCell = MakeCell(status);
    if (installed) {
      for (int c = 0; c < tableAvailableModels_->columnCount(); ++c) {
        if (auto* ci = tableAvailableModels_->item(row, c))
          ci->setFlags(ci->flags() & ~Qt::ItemIsEnabled);
      }
    }
    tableAvailableModels_->setItem(row, 6, stCell);
  }

  applyTableFilter(tableAvailableModels_, filterAvailableModels_->text());
#endif
}

void ResourcePage::populateRemoteProviders(
    const std::vector<vinput::script::RegistryEntry>& providers) {
  tableAvailableProviders_->setRowCount(0);
  CoreConfig config = ConfigManager::Get().Load();
  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto& entry : providers) {
    QString id = QString::fromStdString(entry.id);

    QString titleStr =
        QString::fromStdString(vinput::registry::LookupI18n(i18n_map, entry.id + ".title", ""));
    QString title = titleStr.isEmpty() ? id : titleStr;
    QString desc = QString::fromStdString(
        vinput::registry::LookupI18n(i18n_map, entry.id + ".description", ""));

    int row = tableAvailableProviders_->rowCount();
    tableAvailableProviders_->insertRow(row);
    tableAvailableProviders_->setItem(row, 0, MakeCell(title, id));
    tableAvailableProviders_->setItem(row, 1, MakeCell(desc));
    tableAvailableProviders_->setItem(row, 2,
                                      MakeCell(entry.stream ? tr("stream") : tr("non-stream")));

    bool installed = ResolveAsrProvider(config, entry.id) != nullptr;
    QString status = installed ? tr("installed") : tr("available");

    auto* stCell = MakeCell(status);
    tableAvailableProviders_->setItem(row, 3, stCell);

    const QString readme_url = QString::fromStdString(entry.readme_url);
    if (!readme_url.isEmpty()) {
      auto* item = MakeCell(tr("Open README"), readme_url);
      item->setTextAlignment(Qt::AlignCenter);
      tableAvailableProviders_->setItem(row, 4, item);
    } else {
      auto* emptyCell = MakeCell(QStringLiteral("-"));
      emptyCell->setTextAlignment(Qt::AlignCenter);
      tableAvailableProviders_->setItem(row, 4, emptyCell);
    }
  }

  applyTableFilter(tableAvailableProviders_, filterAvailableProviders_->text());
  updateProviderButtons();
}

void ResourcePage::populateRemoteAdapters(
    const std::vector<vinput::script::RegistryEntry>& adapters) {
  tableAvailableAdapters_->setRowCount(0);
  CoreConfig config = ConfigManager::Get().Load();
  auto i18n_map = I18nCache::Get().GetMap();

  for (const auto& entry : adapters) {
    QString id = QString::fromStdString(entry.id);

    QString titleStr =
        QString::fromStdString(vinput::registry::LookupI18n(i18n_map, entry.id + ".title", ""));
    QString title = titleStr.isEmpty() ? id : titleStr;
    QString desc = QString::fromStdString(
        vinput::registry::LookupI18n(i18n_map, entry.id + ".description", ""));

    int row = tableAvailableAdapters_->rowCount();
    tableAvailableAdapters_->insertRow(row);
    tableAvailableAdapters_->setItem(row, 0, MakeCell(title, id));
    tableAvailableAdapters_->setItem(row, 1, MakeCell(desc));

    bool installed = ResolveLlmAdapter(config, entry.id) != nullptr;
    QString status = installed ? tr("installed") : tr("available");

    auto* stCell = MakeCell(status);
    tableAvailableAdapters_->setItem(row, 2, stCell);

    const QString readme_url = QString::fromStdString(entry.readme_url);
    if (!readme_url.isEmpty()) {
      auto* item = MakeCell(tr("Open README"), readme_url);
      item->setTextAlignment(Qt::AlignCenter);
      tableAvailableAdapters_->setItem(row, 3, item);
    } else {
      auto* emptyCell = MakeCell(QStringLiteral("-"));
      emptyCell->setTextAlignment(Qt::AlignCenter);
      tableAvailableAdapters_->setItem(row, 3, emptyCell);
    }
  }

  applyTableFilter(tableAvailableAdapters_, filterAvailableAdapters_->text());
  updateAdapterButtons();
}

void ResourcePage::refreshAll() {
  CoreConfig config = ConfigManager::Get().Load();
  QString baseDir = QString::fromStdString(ResolveModelBaseDir(config).string());
  QPointer<ResourcePage> self(this);
  const uint64_t generation = ++refreshGeneration_;
  refreshWaitingForI18n_ = false;
  i18nWaitGeneration_ = 0;

  // Keep installed models visible during the network round-trip.
#if VINPUT_ENABLE_LOCAL_ASR
  ModelManager local_manager(baseDir.toStdString());
  populateLocalModels(local_manager.ListDetailed(ResolvePreferredLocalModel(config)));
#endif

  btnRefreshResources_->setEnabled(false);
  textLog_->append(tr("Fetching remote registry..."));

  QThreadPool::globalInstance()->start([self, config, baseDir, generation]() {
    if (!self) {
      return;
    }

    std::vector<std::string> warnings;
    std::string models_error;
    std::string providers_error;
    std::string adapters_error;

#if VINPUT_ENABLE_LOCAL_ASR
    const ModelRepository repo(baseDir.toStdString());
    auto remote_models = repo.FetchRegistry(config, ResolveModelRegistryUrls(config), &models_error,
                                            nullptr, &warnings);
#else
    std::vector<RemoteModelEntry> remote_models;
#endif
    auto remote_providers = vinput::script::FetchRegistry(
        config, vinput::script::Kind::kAsrProvider, ResolveAsrProviderRegistryUrls(config),
        &providers_error, nullptr, &warnings);
    auto remote_adapters = vinput::script::FetchRegistry(config, vinput::script::Kind::kLlmAdapter,
                                                         ResolveLlmAdapterRegistryUrls(config),
                                                         &adapters_error, nullptr, &warnings);

    std::sort(warnings.begin(), warnings.end());
    warnings.erase(std::unique(warnings.begin(), warnings.end()), warnings.end());

    QMetaObject::invokeMethod(
        self,
        [self, generation, remote_models = std::move(remote_models),
         remote_providers = std::move(remote_providers),
         remote_adapters = std::move(remote_adapters), models_error = std::move(models_error),
         providers_error = std::move(providers_error), adapters_error = std::move(adapters_error),
         warnings = std::move(warnings)]() mutable {
          if (!self || generation != self->refreshGeneration_) {
            return;
          }

          self->remoteModels_ = std::move(remote_models);
          self->remoteProviders_ = std::move(remote_providers);
          self->remoteAdapters_ = std::move(remote_adapters);

          if (!models_error.empty()) {
            self->textLog_->append(ResourcePage::tr("Models fetch error: %1")
                                       .arg(QString::fromStdString(models_error)));
          }
          if (!providers_error.empty()) {
            self->textLog_->append(ResourcePage::tr("Providers fetch error: %1")
                                       .arg(QString::fromStdString(providers_error)));
          }
          if (!adapters_error.empty()) {
            self->textLog_->append(ResourcePage::tr("Adapters fetch error: %1")
                                       .arg(QString::fromStdString(adapters_error)));
          }
          for (const auto& warning : warnings) {
            self->textLog_->append(
                ResourcePage::tr("Registry warning: %1").arg(QString::fromStdString(warning)));
          }

          // Paint remotes with the current map first so rows appear even
          // when disk i18n is already warm; finishRefreshAfterI18n repaints
          // after ReloadFromDisk if titles changed.
          self->populateRemoteModels(self->remoteModels_);
          self->populateRemoteProviders(self->remoteProviders_);
          self->populateRemoteAdapters(self->remoteAdapters_);

          self->refreshWaitingForI18n_ = true;
          self->i18nWaitGeneration_ = I18nCache::Get().ReloadFromDisk();
        },
        Qt::QueuedConnection);
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
#if !VINPUT_ENABLE_LOCAL_ASR
  return;
#else
  if (tableInstalledModels_ == nullptr) {
    return;
  }
  auto items = tableInstalledModels_->selectedItems();
  if (items.isEmpty())
    return;
  auto* name_item = tableInstalledModels_->item(tableInstalledModels_->currentRow(), 0);
  QString model_id = name_item->data(Qt::UserRole).toString();
  QString model_title = name_item->text();
  if (model_title.isEmpty()) {
    model_title = model_id;
  }

  CoreConfig config = ConfigManager::Get().Load();
  std::string err;
  if (!SetPreferredLocalModel(&config, model_id.toStdString(), &err)) {
    QMessageBox::critical(this, tr("Error"), QString::fromStdString(err));
    return;
  }
  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  refreshAll();
  emit configChanged();
  RunReloadAsrBackendAsync(this, [this, model_title](bool ok, const std::string& err) {
    if (!ok) {
      QMessageBox::warning(this, tr("Warning"),
                           tr("Config saved, but failed to reload ASR backend: %1")
                               .arg(QString::fromStdString(err)));
      return;
    }
    textLog_->append(tr("Selected model '%1' saved as the preferred local ASR model. "
                        "Backend reload is in progress.")
                         .arg(model_title));
  });
#endif
}

void ResourcePage::onRemoveModelClicked() {
#if !VINPUT_ENABLE_LOCAL_ASR
  return;
#else
  if (tableInstalledModels_ == nullptr) {
    return;
  }
  auto items = tableInstalledModels_->selectedItems();
  if (items.isEmpty())
    return;
  auto* name_item = tableInstalledModels_->item(tableInstalledModels_->currentRow(), 0);
  QString model_id = name_item->data(Qt::UserRole).toString();
  QString model_title = name_item->text();
  if (model_title.isEmpty()) {
    model_title = model_id;
  }

  auto response = QMessageBox::question(
      this, tr("Confirm"), tr("Are you sure you want to remove model '%1'?").arg(model_title));
  if (response == QMessageBox::Yes) {
    CoreConfig config = ConfigManager::Get().Load();
    const bool reload_backend = ResolvePreferredLocalModel(config) == model_id.toStdString();
    ModelManager manager(ResolveModelBaseDir(config).string());
    std::string err;
    if (!manager.Remove(model_id.toStdString(), &err)) {
      QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
      return;
    }

    // Check if was preferred
    if (reload_backend) {
      SetPreferredLocalModel(&config, "", &err);
      if (!ConfigManager::Get().Save(config)) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
        return;
      }
    }
    textLog_->append(tr("Removed %1.").arg(model_title));
    refreshAll();
    emit configChanged();
    if (reload_backend) {
      RunReloadAsrBackendAsync(this, [this](bool ok, const std::string& err) {
        if (!ok) {
          QMessageBox::warning(this, tr("Error"),
                               tr("Config saved, but failed to reload ASR backend: %1")
                                   .arg(QString::fromStdString(err)));
        }
      });
    }
  }
#endif
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
  if (btnDownloadModel_ != nullptr) {
    btnDownloadModel_->setEnabled(true);
  }
  if (btnRemoveModel_ != nullptr) {
    btnRemoveModel_->setEnabled(true);
  }
  downloadStatusLabel_->clear();
  downloadStatusLabel_->setVisible(false);
  downloadProgressBar_->setValue(0);
  downloadProgressBar_->setVisible(false);
  if (downloadWorker_) {
    downloadWorker_->deleteLater();
    downloadWorker_ = nullptr;
  }
  refreshAll();
  updateProviderButtons();
  updateAdapterButtons();
  emit configChanged();
}

void ResourcePage::onDownloadModelClicked() {
#if !VINPUT_ENABLE_LOCAL_ASR
  return;
#else
  if (tableAvailableModels_ == nullptr) {
    return;
  }
  auto items = tableAvailableModels_->selectedItems();
  if (items.isEmpty())
    return;
  auto* name_item = tableAvailableModels_->item(tableAvailableModels_->currentRow(), 0);
  QString model_id = name_item->data(Qt::UserRole).toString();
  QString model_title = name_item->text();
  if (model_title.isEmpty()) {
    model_title = model_id;
  }

  abortDownload();
  btnDownloadModel_->setEnabled(false);
  btnRemoveModel_->setEnabled(false);
  downloadStatusLabel_->setText(tr("Preparing download..."));
  downloadStatusLabel_->setVisible(true);
  downloadProgressBar_->setValue(0);
  downloadProgressBar_->setVisible(true);

  downloadWorker_ = new DownloadWorker(this);
  connect(downloadWorker_, &DownloadWorker::progress, this, &ResourcePage::onDownloadProgress,
          Qt::QueuedConnection);
  connect(downloadWorker_, &DownloadWorker::error, this, &ResourcePage::onDownloadError);
  connect(downloadWorker_, &QThread::finished, this, &ResourcePage::onDownloadFinished);

  CoreConfig config = ConfigManager::Get().Load();

  downloadWorker_->SetTask([config, model_id, worker = downloadWorker_](std::string* err) -> bool {
    ModelRepository repo(ResolveModelBaseDir(config).string());
    auto urls = ResolveModelRegistryUrls(config);
    return repo.InstallModel(
        config, urls, model_id.toStdString(),
        [worker](const InstallProgress& p) {
          if (p.total_bytes > 0) {
            int percent = static_cast<int>((p.downloaded_bytes * 100) / p.total_bytes);
            QString speed = QString::fromStdString(
                                vinput::str::FormatSize(static_cast<uint64_t>(p.speed_bps))) +
                            "/s";
            worker->ReportProgress(percent, speed);
          }
        },
        err);
  });
  textLog_->append(tr("Starting download for %1...").arg(model_title));
  downloadWorker_->start();
#endif
}

void ResourcePage::onAddProviderClicked() {
  auto items = tableAvailableProviders_->selectedItems();
  if (items.isEmpty())
    return;
  auto* name_item = tableAvailableProviders_->item(tableAvailableProviders_->currentRow(), 0);
  QString id = name_item->data(Qt::UserRole).toString();
  QString title = name_item->text();
  if (title.isEmpty()) {
    title = id;
  }

  abortDownload();
  btnAddProvider_->setEnabled(false);

  downloadWorker_ = new DownloadWorker(this);
  connect(downloadWorker_, &DownloadWorker::error, this, &ResourcePage::onDownloadError);
  connect(downloadWorker_, &QThread::finished, this, [&]() { onDownloadFinished(); });

  CoreConfig config = ConfigManager::Get().Load();

  downloadWorker_->SetTask([config, id](std::string* err) -> bool {
    auto urls = ResolveAsrProviderRegistryUrls(config);
    auto entries =
        vinput::script::FetchRegistry(config, vinput::script::Kind::kAsrProvider, urls, err);
    if (!err->empty())
      return false;

    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const auto& e) { return e.id == id.toStdString(); });
    if (it == entries.end()) {
      *err = "Provider not found in registry.";
      return false;
    }

    std::filesystem::path scriptPath;
    if (!vinput::script::DownloadScript(*it, vinput::script::Kind::kAsrProvider, &scriptPath,
                                        err)) {
      return false;
    }

    CoreConfig mutConfig = config;
    if (!vinput::script::MaterializeAsrProvider(&mutConfig, *it, scriptPath, err)) {
      return false;
    }
    return ConfigManager::Get().Save(mutConfig);
  });
  textLog_->append(tr("Installing provider %1...").arg(title));
  downloadWorker_->start();
}

void ResourcePage::onAddAdapterClicked() {
  auto items = tableAvailableAdapters_->selectedItems();
  if (items.isEmpty())
    return;
  auto* name_item = tableAvailableAdapters_->item(tableAvailableAdapters_->currentRow(), 0);
  QString id = name_item->data(Qt::UserRole).toString();
  QString title = name_item->text();
  if (title.isEmpty()) {
    title = id;
  }

  abortDownload();
  btnAddAdapter_->setEnabled(false);

  downloadWorker_ = new DownloadWorker(this);
  connect(downloadWorker_, &DownloadWorker::error, this, &ResourcePage::onDownloadError);
  connect(downloadWorker_, &QThread::finished, this, &ResourcePage::onDownloadFinished);

  CoreConfig config = ConfigManager::Get().Load();

  downloadWorker_->SetTask([config, id](std::string* err) -> bool {
    auto urls = ResolveLlmAdapterRegistryUrls(config);
    auto entries =
        vinput::script::FetchRegistry(config, vinput::script::Kind::kLlmAdapter, urls, err);
    if (!err->empty())
      return false;

    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const auto& e) { return e.id == id.toStdString(); });
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
  textLog_->append(tr("Installing adapter %1...").arg(title));
  downloadWorker_->start();
}

void ResourcePage::updateProviderButtons() {
  const int row = tableAvailableProviders_->currentRow();
  if (row < 0 || row >= tableAvailableProviders_->rowCount()) {
    btnAddProvider_->setEnabled(false);
    btnRemoveProvider_->setEnabled(false);
    return;
  }
  auto* item = tableAvailableProviders_->item(row, 0);
  if (item == nullptr) {
    btnAddProvider_->setEnabled(false);
    btnRemoveProvider_->setEnabled(false);
    return;
  }
  const QString id = item->data(Qt::UserRole).toString();
  const CoreConfig config = ConfigManager::Get().Load();
  const bool installed = ResolveAsrProvider(config, id.toStdString()) != nullptr;
  btnAddProvider_->setEnabled(!installed);
  btnRemoveProvider_->setEnabled(installed);
}

void ResourcePage::updateAdapterButtons() {
  const int row = tableAvailableAdapters_->currentRow();
  if (row < 0 || row >= tableAvailableAdapters_->rowCount()) {
    btnAddAdapter_->setEnabled(false);
    btnRemoveAdapter_->setEnabled(false);
    return;
  }
  auto* item = tableAvailableAdapters_->item(row, 0);
  if (item == nullptr) {
    btnAddAdapter_->setEnabled(false);
    btnRemoveAdapter_->setEnabled(false);
    return;
  }
  const QString id = item->data(Qt::UserRole).toString();
  const CoreConfig config = ConfigManager::Get().Load();
  const bool installed = ResolveLlmAdapter(config, id.toStdString()) != nullptr;
  btnAddAdapter_->setEnabled(!installed);
  btnRemoveAdapter_->setEnabled(installed);
}

void ResourcePage::onRemoveProviderClicked() {
  const int row = tableAvailableProviders_->currentRow();
  if (row < 0 || row >= tableAvailableProviders_->rowCount()) {
    return;
  }
  auto* name_item = tableAvailableProviders_->item(row, 0);
  if (name_item == nullptr) {
    return;
  }
  const QString id = name_item->data(Qt::UserRole).toString();
  QString title = name_item->text();
  if (title.isEmpty()) {
    title = id;
  }

  const auto response = QMessageBox::question(
      this, tr("Confirm"), tr("Are you sure you want to remove ASR provider '%1'?").arg(title));
  if (response != QMessageBox::Yes) {
    return;
  }

  CoreConfig config = ConfigManager::Get().Load();
  auto it =
      std::remove_if(config.asr.providers.begin(), config.asr.providers.end(),
                     [&](const AsrProvider& p) { return AsrProviderId(p) == id.toStdString(); });
  if (it == config.asr.providers.end()) {
    return;
  }
  if (std::holds_alternative<LocalAsrProvider>(*it)) {
    QMessageBox::warning(this, tr("Error"), tr("The local ASR provider cannot be removed."));
    return;
  }
  config.asr.providers.erase(it, config.asr.providers.end());
  const bool was_active = (config.asr.activeProvider == id.toStdString());
  if (was_active) {
    config.asr.activeProvider.clear();
  }

  const auto managed_path =
      vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kAsrProvider, id.toStdString());
  if (!managed_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(managed_path, ec);
  }

  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  textLog_->append(tr("Removed ASR provider %1.").arg(title));
  refreshAll();
  emit configChanged();
  if (was_active) {
    RunReloadAsrBackendAsync(this, [this](bool ok, const std::string& err) {
      if (!ok) {
        QMessageBox::warning(this, tr("Warning"),
                             tr("Config saved, but failed to reload ASR backend: %1")
                                 .arg(QString::fromStdString(err)));
      }
    });
  }
}

void ResourcePage::onRemoveAdapterClicked() {
  const int row = tableAvailableAdapters_->currentRow();
  if (row < 0 || row >= tableAvailableAdapters_->rowCount()) {
    return;
  }
  auto* name_item = tableAvailableAdapters_->item(row, 0);
  if (name_item == nullptr) {
    return;
  }
  const QString id = name_item->data(Qt::UserRole).toString();
  QString title = name_item->text();
  if (title.isEmpty()) {
    title = id;
  }

  const auto response = QMessageBox::question(
      this, tr("Confirm"), tr("Are you sure you want to remove LLM adapter '%1'?").arg(title));
  if (response != QMessageBox::Yes) {
    return;
  }

  if (vinput::adapter::IsRunning(id.toStdString())) {
    vinput::cli::DbusClient dbus;
    std::string err;
    dbus.StopAdapter(id.toStdString(), &err);
  }

  CoreConfig config = ConfigManager::Get().Load();
  auto it = std::remove_if(config.llm.adapters.begin(), config.llm.adapters.end(),
                           [&](const LlmAdapter& a) { return a.id == id.toStdString(); });
  if (it == config.llm.adapters.end()) {
    return;
  }
  config.llm.adapters.erase(it, config.llm.adapters.end());

  const auto managed_path =
      vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kLlmAdapter, id.toStdString());
  if (!managed_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(managed_path, ec);
  }

  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  textLog_->append(tr("Removed LLM adapter %1.").arg(title));
  refreshAll();
  emit configChanged();
}

} // namespace vinput::gui
