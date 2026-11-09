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
struct RegistryFetchResults;

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
  void populateRemoteTriggers(const std::vector<vinput::script::RegistryEntry>& triggers);
  void populateRemoteReceivers(const std::vector<vinput::script::RegistryEntry>& receivers);
  void applyRegistryFetchResults(RegistryFetchResults results, uint64_t generation);
  void refreshLocalizedTables();
  void finishRefreshAfterI18n(const QString& error = {}, uint64_t generation = 0);

  void abortDownload();
  void startScriptDownloadTask(QTableWidget* table, QPushButton* action_button,
                               vinput::script::Kind kind, bool is_update,
                               const QString& log_format);
  template <typename MutateFn, typename PostRemoveFn>
  void removeScriptItem(QTableWidget* table, vinput::script::Kind kind,
                        const QString& confirm_question, const QString& removed_log,
                        const MutateFn& mutate_fn, const PostRemoveFn& post_remove_fn);
  template <typename FindClientFn>
  void editClientItem(QTableWidget* table, const QString& dialog_title, const QString& updated_log,
                      const FindClientFn& find_client_fn);

  QLineEdit* filterInstalledModels_;
  QLineEdit* filterAvailableModels_;
  QLineEdit* filterAvailableProviders_;
  QLineEdit* filterAvailableAdapters_;
  QLineEdit* filterAvailableTriggers_;
  QLineEdit* filterAvailableReceivers_;
  QTabWidget* resourceTabs_;
  QTableWidget* tableInstalledModels_;
  QTableWidget* tableAvailableModels_;
  QTableWidget* tableAvailableProviders_;
  QTableWidget* tableAvailableAdapters_;
  QTableWidget* tableAvailableTriggers_;
  QTableWidget* tableAvailableReceivers_;
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
  QPushButton* btnAddTrigger_;
  QPushButton* btnUpdateTrigger_;
  QPushButton* btnEditTrigger_;
  QPushButton* btnRemoveTrigger_;
  QPushButton* btnAddReceiver_;
  QPushButton* btnUpdateReceiver_;
  QPushButton* btnEditReceiver_;
  QPushButton* btnRemoveReceiver_;
  QPushButton* btnRefreshResources_;
  QPointer<DownloadWorker> downloadWorker_;
  std::vector<RemoteModelEntry> remoteModels_;
  std::vector<vinput::script::RegistryEntry> remoteProviders_;
  std::vector<vinput::script::RegistryEntry> remoteAdapters_;
  std::vector<vinput::script::RegistryEntry> remoteTriggers_;
  std::vector<vinput::script::RegistryEntry> remoteReceivers_;
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
  void onAddTriggerClicked();
  void onUpdateTriggerClicked();
  void onEditTriggerClicked();
  void onRemoveTriggerClicked();
  void onAddReceiverClicked();
  void onUpdateReceiverClicked();
  void onEditReceiverClicked();
  void onRemoveReceiverClicked();
  void updateProviderButtons();
  void updateAdapterButtons();
  void updateTriggerButtons();
  void updateReceiverButtons();
  void onDownloadProgress(int percent, const QString& speed);
  void onDownloadError(const QString& msg);
  void onDownloadFinished(DownloadWorker* originating_worker = nullptr);
};

} // namespace vinput::gui
