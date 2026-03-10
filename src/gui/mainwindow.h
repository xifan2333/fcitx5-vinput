#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QMainWindow>
#include <QPushButton>
#include <QTabWidget>

#include "common/core_config.h"

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override = default;

private slots:
  void onSaveClicked();
  void onOpenConfigClicked();
  void loadConfigToUi();

  // Model Management Slots
  void refreshModelList();
  void onUseModelClicked();
  void onRemoveModelClicked();
  void onDownloadModelClicked();
  void onProcessReadyReadStandardOutput();
  void onProcessReadyReadStandardError();
  void onProcessFinished(int exitCode, int exitStatus);

private:
  void setupUi();
  void setupGeneralTab();
  void setupModelTab();

  QTabWidget *tabWidget;
  QWidget *generalTab;
  QWidget *modelTab;

  // UI Elements - General
  QComboBox *comboDevice;
  QComboBox *comboModel;
  QComboBox *comboScene;
  QCheckBox *checkLlmEnabled;
  QPushButton *btnOpenConfig;
  QPushButton *btnSave;

  // Core Data
  CoreConfig currentConfig;

  // UI Elements - Model Management
  class QListWidget *listModels;
  class QLineEdit *editDownloadUrl;
  class QTextEdit *textLog;
  QPushButton *btnUseModel;
  QPushButton *btnRemoveModel;
  QPushButton *btnDownloadModel;

  class QProcess *cliProcess;
};
