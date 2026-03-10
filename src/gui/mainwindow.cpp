#include "mainwindow.h"
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QUrl>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setupUi();
  loadConfigToUi();
}

void MainWindow::setupUi() {
  auto *centralWidget = new QWidget(this);
  setCentralWidget(centralWidget);

  auto *mainLayout = new QVBoxLayout(centralWidget);

  tabWidget = new QTabWidget(this);
  mainLayout->addWidget(tabWidget);

  setupGeneralTab();
  setupModelTab();

  auto *bottomLayout = new QHBoxLayout();
  btnSave = new QPushButton(tr("Save Settings"), this);
  connect(btnSave, &QPushButton::clicked, this, &MainWindow::onSaveClicked);

  bottomLayout->addStretch();
  bottomLayout->addWidget(btnSave);
  mainLayout->addLayout(bottomLayout);
}

void MainWindow::setupGeneralTab() {
  generalTab = new QWidget();
  auto *layout = new QVBoxLayout(generalTab);

  auto *formLayout = new QFormLayout();

  comboDevice = new QComboBox();
  comboDevice->setEditable(true); // Allow custom device strings for now
  formLayout->addRow(tr("Capture Device:"), comboDevice);

  comboModel = new QComboBox();
  formLayout->addRow(tr("Active Model:"), comboModel);

  comboScene = new QComboBox();
  formLayout->addRow(tr("Active Scene:"), comboScene);

  checkLlmEnabled = new QCheckBox(tr("Enable LLM Global Features"));
  formLayout->addRow(tr("LLM:"), checkLlmEnabled);

  layout->addLayout(formLayout);

  layout->addSpacing(20);

  btnOpenConfig = new QPushButton(tr("Open Advanced Config (config.json)"));
  connect(btnOpenConfig, &QPushButton::clicked, this,
          &MainWindow::onOpenConfigClicked);
  layout->addWidget(btnOpenConfig);

  layout->addStretch();
  tabWidget->addTab(generalTab, tr("General Settings"));
}

#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QTextEdit>
#include <QTimer>

void MainWindow::setupModelTab() {
  modelTab = new QWidget();
  auto *layout = new QVBoxLayout(modelTab);

  // List and Action Buttons
  auto *listLayout = new QHBoxLayout();
  listModels = new QListWidget();
  listLayout->addWidget(listModels);

  auto *btnLayout = new QVBoxLayout();
  btnUseModel = new QPushButton(tr("Use Selected"));
  btnRemoveModel = new QPushButton(tr("Remove Selected"));
  btnLayout->addWidget(btnUseModel);
  btnLayout->addWidget(btnRemoveModel);
  btnLayout->addStretch();
  listLayout->addLayout(btnLayout);

  layout->addLayout(listLayout);

  // Download Area
  auto *lblDownload = new QLabel(tr("Download Model (URL or Registry Name):"));
  layout->addWidget(lblDownload);

  auto *dlLayout = new QHBoxLayout();
  editDownloadUrl = new QLineEdit();
  btnDownloadModel = new QPushButton(tr("Download"));
  dlLayout->addWidget(editDownloadUrl);
  dlLayout->addWidget(btnDownloadModel);
  layout->addLayout(dlLayout);

  // Log Area
  textLog = new QTextEdit();
  textLog->setReadOnly(true);
  layout->addWidget(textLog);

  tabWidget->addTab(modelTab, tr("Model Management"));

  // Connect Slots
  connect(btnUseModel, &QPushButton::clicked, this,
          &MainWindow::onUseModelClicked);
  connect(btnRemoveModel, &QPushButton::clicked, this,
          &MainWindow::onRemoveModelClicked);
  connect(btnDownloadModel, &QPushButton::clicked, this,
          &MainWindow::onDownloadModelClicked);

  cliProcess = new QProcess(this);
  connect(cliProcess, &QProcess::readyReadStandardOutput, this,
          &MainWindow::onProcessReadyReadStandardOutput);
  connect(cliProcess, &QProcess::readyReadStandardError, this,
          &MainWindow::onProcessReadyReadStandardError);
  connect(cliProcess,
          QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          &MainWindow::onProcessFinished);

  // Initial Refresh
  QTimer::singleShot(0, this, &MainWindow::refreshModelList);
}

void MainWindow::refreshModelList() {
  listModels->clear();
  QDir modelDir(
      QString::fromStdString(ResolveModelBaseDir(currentConfig).string()));
  if (modelDir.exists()) {
    QStringList filters;
    filters << "*";
    modelDir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList dirs = modelDir.entryList();
    listModels->addItems(dirs);
  }
}

void MainWindow::onUseModelClicked() {
  if (auto item = listModels->currentItem()) {
    comboModel->setCurrentText(item->text());
    tabWidget->setCurrentWidget(generalTab);
  }
}

void MainWindow::onRemoveModelClicked() {
  if (auto item = listModels->currentItem()) {
    auto response = QMessageBox::question(
        this, tr("Confirm"),
        tr("Are you sure you want to remove model '%1'?").arg(item->text()));
    if (response == QMessageBox::Yes) {
      btnDownloadModel->setEnabled(false);
      btnRemoveModel->setEnabled(false);
      textLog->append("Removing " + item->text() + "...");
      cliProcess->start("vinput", QStringList()
                                      << "model" << "remove" << item->text());
    }
  }
}

void MainWindow::onDownloadModelClicked() {
  QString target = editDownloadUrl->text().trimmed();
  if (!target.isEmpty()) {
    btnDownloadModel->setEnabled(false);
    btnRemoveModel->setEnabled(false);
    textLog->append("Downloading " + target + "...");
    cliProcess->start("vinput", QStringList() << "model" << "add" << target);
  }
}

void MainWindow::onProcessReadyReadStandardOutput() {
  textLog->append(
      QString::fromUtf8(cliProcess->readAllStandardOutput()).trimmed());
}

void MainWindow::onProcessReadyReadStandardError() {
  textLog->append(
      QString::fromUtf8(cliProcess->readAllStandardError()).trimmed());
}

void MainWindow::onProcessFinished(int exitCode, int exitStatus) {
  textLog->append(tr("Process finished"));
  btnDownloadModel->setEnabled(true);
  btnRemoveModel->setEnabled(true);
  refreshModelList();
  loadConfigToUi();
}

void MainWindow::loadConfigToUi() {
  currentConfig = LoadCoreConfig();

  // Populate Device (Ideally query pipewire here, but for now just use current
  // + "default")
  comboDevice->clear();
  comboDevice->addItem("default");
  if (currentConfig.captureDevice != "default" &&
      !currentConfig.captureDevice.empty()) {
    comboDevice->addItem(QString::fromStdString(currentConfig.captureDevice));
  }
  comboDevice->setCurrentText(
      QString::fromStdString(currentConfig.captureDevice));

  // Populate Models (Query from directory)
  comboModel->clear();
  QDir modelDir(
      QString::fromStdString(ResolveModelBaseDir(currentConfig).string()));
  if (modelDir.exists()) {
    QStringList filters;
    filters << "*";
    modelDir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList dirs = modelDir.entryList();
    comboModel->addItems(dirs);
  }
  if (!currentConfig.activeModel.empty()) {
    if (comboModel->findText(
            QString::fromStdString(currentConfig.activeModel)) == -1) {
      comboModel->addItem(QString::fromStdString(currentConfig.activeModel));
    }
    comboModel->setCurrentText(
        QString::fromStdString(currentConfig.activeModel));
  }

  // Populate Scenes
  comboScene->clear();
  comboScene->addItem("default");
  for (const auto &scene : currentConfig.scenes.definitions) {
    if (scene.id != "default" && !scene.id.empty()) {
      comboScene->addItem(QString::fromStdString(scene.id));
    }
  }
  if (!currentConfig.scenes.activeScene.empty()) {
    comboScene->setCurrentText(
        QString::fromStdString(currentConfig.scenes.activeScene));
  }

  // LLM
  checkLlmEnabled->setChecked(currentConfig.llm.enabled);
}

void MainWindow::onSaveClicked() {
  currentConfig.captureDevice = comboDevice->currentText().toStdString();
  currentConfig.activeModel = comboModel->currentText().toStdString();
  currentConfig.scenes.activeScene = comboScene->currentText().toStdString();
  currentConfig.llm.enabled = checkLlmEnabled->isChecked();

  if (SaveCoreConfig(currentConfig)) {
    // notify fcitx5 daemon to load new config via dbus
    QProcess::startDetached("vinput", QStringList() << "daemon" << "reload");
    QMessageBox::information(this, tr("Success"),
                             tr("Settings saved successfully!"));
    close();
  } else {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
  }
}

void MainWindow::onOpenConfigClicked() {
  QString configPath = QString::fromStdString(GetCoreConfigPath());
  QDesktopServices::openUrl(QUrl::fromLocalFile(configPath));
}
