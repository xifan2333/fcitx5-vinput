#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>

#include "common/core_config.h"

class QProcess;
class QThread;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

private slots:
  void onSaveClicked();
  void onOpenConfigClicked();
  void loadConfigToUi();

  // Model Management
  void refreshModelList();
  void onUseModelClicked();
  void onRemoveModelClicked();
  void onDownloadModelClicked();
  void onInstallProgress(quint64 downloaded, quint64 total, double speed);
  void onInstallFinished(bool success, const QString &error);
  void onProcessReadyReadStandardOutput();
  void onProcessReadyReadStandardError();
  void onProcessFinished(int exitCode, int exitStatus);

  // Scene Management
  void onSceneAdd();
  void onSceneEdit();
  void onSceneRemove();
  void onSceneSetActive();

  // LLM Provider Management
  void onLlmAdd();
  void onLlmEdit();
  void onLlmRemove();
  void onLlmSetActive();

private:
  void setupUi();
  void setupGeneralTab();
  void setupModelTab();
  void setupSceneTab();
  void setupLlmTab();
  void refreshSceneList();
  void refreshLlmList();

  QTabWidget *tabWidget;
  QWidget *generalTab;
  QWidget *modelTab;
  QWidget *sceneTab;
  QWidget *llmTab;

  // General Tab
  QComboBox *comboDevice;
  QComboBox *comboModel;
  QCheckBox *checkLlmEnabled;
  QSpinBox *spinCandidateCount;
  QComboBox *comboActiveScene;
  QPushButton *btnOpenConfig;
  QPushButton *btnSave;

  // Model Tab
  QListWidget *listModels;
  QListWidget *listRemoteModels;
  QTextEdit *textLog;
  QPushButton *btnUseModel;
  QPushButton *btnRemoveModel;
  QPushButton *btnDownloadModel;
  QPushButton *btnRefreshModels;

  // Scene Tab
  QListWidget *listScenes;
  QPushButton *btnSceneAdd;
  QPushButton *btnSceneEdit;
  QPushButton *btnSceneRemove;
  QPushButton *btnSceneSetActive;

  // LLM Tab
  QListWidget *listProviders;
  QPushButton *btnLlmAdd;
  QPushButton *btnLlmEdit;
  QPushButton *btnLlmRemove;
  QPushButton *btnLlmSetActive;

  // Core Data
  CoreConfig currentConfig;

  // CLI Process
  QProcess *cliProcess = nullptr;

  // Worker
  class ModelInstallWorker *installWorker_ = nullptr;
};
