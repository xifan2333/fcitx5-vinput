#pragma once

#include <QtCore/qobjectdefs.h>
#include <QtCore/qpointer.h>
#include <QtCore/qstring.h>
#include <QtCore/qtmetamacros.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qprogressbar.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qtablewidget.h>
#include <QtWidgets/qtabwidget.h>
#include <QtWidgets/qtextedit.h>
#include <QtWidgets/qwidget.h>
#include <cstdint>
#include <vector>

#include "common/asr/model_manager.h"
#include "common/registry/registry_models.h"
#include "common/registry/registry_scripts.h"

namespace vinput::gui {

class DownloadWorker;

class ResourcePage : public QWidget {
  Q_OBJECT

public:
  explicit ResourcePage(QWidget* parent = nullptr);
  ~ResourcePage() override;

  ResourcePage(const ResourcePage&) = delete;
  ResourcePage& operator=(const ResourcePage&) = delete;
  ResourcePage(ResourcePage&&) = delete;
  ResourcePage& operator=(ResourcePage&&) = delete;

  void reload();

private:
  void populateLocalModels(const std::vector<ModelSummary>& models);
  void populateRemoteModels(const std::vector<RemoteModelEntry>& models);
  void populateRemoteProviders(const std::vector<vinput::script::RegistryEntry>& providers);
  void populateRemoteAdapters(const std::vector<vinput::script::RegistryEntry>& adapters);
  static void applyTableFilter(QTableWidget* table, const QString& filter_text);
  void refreshLocalizedTables();
  void finishRefreshAfterI18n(const QString& error = {}, uint64_t generation = 0);

  void abortDownload();

  QLineEdit* filterInstalledModels_;
  QLineEdit* filterAvailableModels_;
  QLineEdit* filterAvailableProviders_;
  QLineEdit* filterAvailableAdapters_;
  QTabWidget* resourceTabs_;
  QTableWidget* tableInstalledModels_;
  QTableWidget* tableAvailableModels_;
  QTableWidget* tableAvailableProviders_;
  QTableWidget* tableAvailableAdapters_;
  QLabel* downloadStatusLabel_;
  QProgressBar* downloadProgressBar_;
  QTextEdit* textLog_;
  QPushButton* btnUseModel_;
  QPushButton* btnRemoveModel_;
  QPushButton* btnDownloadModel_;
  QPushButton* btnAddProvider_;
  QPushButton* btnUpdateProvider_;
  QPushButton* btnRemoveProvider_;
  QPushButton* btnAddAdapter_;
  QPushButton* btnUpdateAdapter_;
  QPushButton* btnRemoveAdapter_;
  QPushButton* btnRefreshResources_;
  QPointer<DownloadWorker> downloadWorker_;
  std::vector<RemoteModelEntry> remoteModels_;
  std::vector<vinput::script::RegistryEntry> remoteProviders_;
  std::vector<vinput::script::RegistryEntry> remoteAdapters_;
  uint64_t refreshGeneration_ = 0;
  uint64_t i18nWaitGeneration_ = 0;
  bool refreshWaitingForI18n_ = false;

signals:
  void configChanged();

private slots:
  void refreshAll();
  void onUseModelClicked();
  void onRemoveModelClicked();
  void onDownloadModelClicked();
  void onAddProviderClicked();
  void onUpdateProviderClicked();
  void onRemoveProviderClicked();
  void onAddAdapterClicked();
  void onUpdateAdapterClicked();
  void onRemoveAdapterClicked();
  void updateProviderButtons();
  void updateAdapterButtons();
  void onDownloadProgress(int percent, const QString& speed);
  void onDownloadError(const QString& msg);
  void onDownloadFinished(DownloadWorker* originating_worker = nullptr);
};

} // namespace vinput::gui
