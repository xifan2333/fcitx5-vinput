#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QWidget>
#include <cstdint>

#include "common/asr/model_manager.h"
#include "common/registry/registry_models.h"
#include "common/registry/registry_scripts.h"

namespace vinput::gui {

class DownloadWorker;

class ResourcePage : public QWidget {
  Q_OBJECT

public:
  explicit ResourcePage(QWidget* parent = nullptr);

  void reload();

signals:
  void configChanged();

private slots:
  void refreshAll();
  void onUseModelClicked();
  void onRemoveModelClicked();
  void onDownloadModelClicked();
  void onAddProviderClicked();
  void onRemoveProviderClicked();
  void onAddAdapterClicked();
  void onRemoveAdapterClicked();
  void updateProviderButtons();
  void updateAdapterButtons();
  void onDownloadProgress(int percent, QString speed);
  void onDownloadError(QString msg);
  void onDownloadFinished();

private:
  void populateLocalModels(const std::vector<ModelSummary>& models);
  void populateRemoteModels(const std::vector<RemoteModelEntry>& models);
  void populateRemoteProviders(const std::vector<vinput::script::RegistryEntry>& providers);
  void populateRemoteAdapters(const std::vector<vinput::script::RegistryEntry>& adapters);
  void applyTableFilter(QTableWidget* table, const QString& filter_text);
  void refreshLocalizedTables();
  void finishRefreshAfterI18n(const QString& error = {}, quint64 generation = 0);

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
  QPushButton* btnRemoveProvider_;
  QPushButton* btnAddAdapter_;
  QPushButton* btnRemoveAdapter_;
  QPushButton* btnRefreshResources_;
  DownloadWorker* downloadWorker_ = nullptr;
  std::vector<RemoteModelEntry> remoteModels_;
  std::vector<vinput::script::RegistryEntry> remoteProviders_;
  std::vector<vinput::script::RegistryEntry> remoteAdapters_;
  uint64_t refreshGeneration_ = 0;
  quint64 i18nWaitGeneration_ = 0;
  bool refreshWaitingForI18n_ = false;
};

} // namespace vinput::gui
