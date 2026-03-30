#pragma once

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QWidget>

#include "common/asr/model_manager.h"
#include "common/registry/registry_models.h"
#include "common/registry/registry_scripts.h"

namespace vinput::gui {

class DownloadWorker;

class ResourcePage : public QWidget {
  Q_OBJECT

public:
  explicit ResourcePage(QWidget *parent = nullptr);

  void reload();

signals:
  void configChanged();

private slots:
  void refreshAll();
  void onUseModelClicked();
  void onRemoveModelClicked();
  void onDownloadModelClicked();
  void onAddProviderClicked();
  void onAddAdapterClicked();
  void onDownloadProgress(int percent, QString speed);
  void onDownloadError(QString msg);
  void onDownloadFinished();

private:
  void populateLocalModels(const std::vector<ModelSummary> &models);
  void populateRemoteModels(const std::vector<RemoteModelEntry> &models);
  void populateRemoteProviders(const std::vector<vinput::script::RegistryEntry> &providers);
  void populateRemoteAdapters(const std::vector<vinput::script::RegistryEntry> &adapters);

  void abortDownload();

  QTableWidget *tableInstalledModels_;
  QTableWidget *tableAvailableModels_;
  QTableWidget *tableAvailableProviders_;
  QTableWidget *tableAvailableAdapters_;
  QLabel *downloadStatusLabel_;
  QProgressBar *downloadProgressBar_;
  QTextEdit *textLog_;
  QPushButton *btnUseModel_;
  QPushButton *btnRemoveModel_;
  QPushButton *btnDownloadModel_;
  QPushButton *btnAddProvider_;
  QPushButton *btnAddAdapter_;
  QPushButton *btnRefreshResources_;
  DownloadWorker *downloadWorker_ = nullptr;
};

}  // namespace vinput::gui
