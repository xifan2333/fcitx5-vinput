#include "mainwindow.h"
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(tr("Vinput Configuration"));
  setupUi();
  loadConfigToUi();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi() {
  auto *centralWidget = new QWidget(this);
  setCentralWidget(centralWidget);

  auto *mainLayout = new QVBoxLayout(centralWidget);

  tabWidget = new QTabWidget(this);
  mainLayout->addWidget(tabWidget);

  setupGeneralTab();
  setupModelTab();
  setupSceneTab();
  setupLlmTab();

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
  comboDevice->setEditable(false);
  formLayout->addRow(tr("Capture Device:"), comboDevice);

  comboModel = new QComboBox();
  formLayout->addRow(tr("Active Model:"), comboModel);

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

#include <QListWidget>
#include <QProcess>
#include <QTextEdit>
#include <QTimer>

namespace {

struct DeviceEntry {
  QString name;
  QString description;
};

struct ModelEntry {
  QString name;
  QString display_name;
  QString model_type;
  QString language;
  QString status;
  QString size;
};

bool RunVinputJson(const QStringList &args, QJsonDocument *out_doc,
                   QString *error_out) {
  QString vinput_path = QStandardPaths::findExecutable("vinput");
  if (vinput_path.isEmpty()) {
    if (error_out)
      *error_out = "vinput not found in PATH";
    return false;
  }

  QProcess proc;
  QStringList cmd_args;
  cmd_args << "--json";
  cmd_args << args;
  proc.start(vinput_path, cmd_args);
  if (!proc.waitForFinished(5000)) {
    proc.kill();
    if (error_out)
      *error_out = "vinput command timed out";
    return false;
  }

  if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
    if (error_out)
      *error_out = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    return false;
  }

  QByteArray output = proc.readAllStandardOutput();
  QJsonParseError parse_error;
  QJsonDocument doc = QJsonDocument::fromJson(output, &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    if (error_out)
      *error_out = "invalid JSON output from vinput";
    return false;
  }

  if (out_doc)
    *out_doc = doc;
  return true;
}

QList<DeviceEntry> LoadDevicesFromCli(QString *error_out) {
  QJsonDocument doc;
  if (!RunVinputJson({"device", "list"}, &doc, error_out) ||
      !doc.isArray()) {
    if (error_out && error_out->isEmpty())
      *error_out = "invalid JSON output from vinput device list";
    return {};
  }

  QList<DeviceEntry> devices;
  for (const auto &value : doc.array()) {
    if (!value.isObject())
      continue;
    QJsonObject obj = value.toObject();
    QString name = obj.value("name").toString();
    if (name.isEmpty())
      continue;
    DeviceEntry entry;
    entry.name = name;
    entry.description = obj.value("description").toString();
    devices.push_back(entry);
  }

  return devices;
}

QList<ModelEntry> LoadLocalModelsFromCli(QString *error_out) {
  QJsonDocument doc;
  if (!RunVinputJson({"model", "list"}, &doc, error_out) ||
      !doc.isArray()) {
    if (error_out && error_out->isEmpty())
      *error_out = "invalid JSON output from vinput model list";
    return {};
  }

  QList<ModelEntry> models;
  for (const auto &value : doc.array()) {
    if (!value.isObject())
      continue;
    QJsonObject obj = value.toObject();
    QString name = obj.value("name").toString();
    if (name.isEmpty())
      continue;
    ModelEntry entry;
    entry.name = name;
    entry.model_type = obj.value("model_type").toString();
    entry.language = obj.value("language").toString();
    entry.status = obj.value("status").toString();
    models.push_back(entry);
  }
  return models;
}

QList<ModelEntry> LoadRemoteModelsFromCli(QString *error_out) {
  QJsonDocument doc;
  if (!RunVinputJson({"model", "list", "--remote"}, &doc, error_out) ||
      !doc.isArray()) {
    if (error_out && error_out->isEmpty())
      *error_out = "invalid JSON output from vinput model list --remote";
    return {};
  }

  QList<ModelEntry> models;
  for (const auto &value : doc.array()) {
    if (!value.isObject())
      continue;
    QJsonObject obj = value.toObject();
    QString name = obj.value("name").toString();
    if (name.isEmpty())
      continue;
    ModelEntry entry;
    entry.name = name;
    entry.display_name = obj.value("display_name").toString();
    entry.model_type = obj.value("model_type").toString();
    entry.language = obj.value("language").toString();
    entry.status = obj.value("status").toString();
    entry.size = obj.value("size").toString();
    models.push_back(entry);
  }
  return models;
}

} // namespace

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
  btnRefreshModels = new QPushButton(tr("Refresh"));
  btnLayout->addWidget(btnUseModel);
  btnLayout->addWidget(btnRemoveModel);
  btnLayout->addWidget(btnRefreshModels);
  btnLayout->addStretch();
  listLayout->addLayout(btnLayout);

  layout->addLayout(listLayout);

  // Remote Models
  auto *lblRemote = new QLabel(tr("Remote Models (Registry):"));
  layout->addWidget(lblRemote);

  listRemoteModels = new QListWidget();
  layout->addWidget(listRemoteModels);

  // Download Area
  auto *lblDownload = new QLabel(tr("Download Selected Remote Model:"));
  layout->addWidget(lblDownload);

  btnDownloadModel = new QPushButton(tr("Download"));
  layout->addWidget(btnDownloadModel);

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
  connect(btnRefreshModels, &QPushButton::clicked, this,
          &MainWindow::refreshModelList);
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

  QString local_err;
  QList<ModelEntry> local_models = LoadLocalModelsFromCli(&local_err);
  if (!local_err.isEmpty()) {
    textLog->append(tr("Local model list error: %1").arg(local_err));
  }

  for (const auto &m : local_models) {
    QString status = m.status.isEmpty() ? "installed" : m.status;
    QString label = QString("%1  [%2]").arg(m.name, status);
    if (!m.language.isEmpty() || !m.model_type.isEmpty()) {
      label += QString("  %1 %2").arg(m.model_type, m.language).trimmed();
    }
    auto *item = new QListWidgetItem(label, listModels);
    item->setData(Qt::UserRole, m.name);
  }

  listRemoteModels->clear();
  QString remote_err;
  QList<ModelEntry> remote_models = LoadRemoteModelsFromCli(&remote_err);
  if (!remote_err.isEmpty()) {
    textLog->append(tr("Remote model list error: %1").arg(remote_err));
  }

  for (const auto &m : remote_models) {
    QString display = m.display_name.isEmpty() ? m.name : m.display_name;
    QString label = QString("%1  [%2]").arg(display, m.status);
    if (!m.size.isEmpty())
      label += QString("  %1").arg(m.size);
    if (!m.model_type.isEmpty() || !m.language.isEmpty()) {
      label += QString("  %1 %2").arg(m.model_type, m.language).trimmed();
    }
    if (!m.display_name.isEmpty() && m.display_name != m.name)
      label += QString("  (%1)").arg(m.name);

    auto *item = new QListWidgetItem(label, listRemoteModels);
    item->setData(Qt::UserRole, m.name);
    if (m.status == "installed")
      item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
  }
}

void MainWindow::onUseModelClicked() {
  if (auto item = listModels->currentItem()) {
    QString model_name = item->data(Qt::UserRole).toString();
    if (model_name.isEmpty())
      model_name = item->text();
    comboModel->setCurrentText(model_name);
    tabWidget->setCurrentWidget(generalTab);
  }
}

void MainWindow::onRemoveModelClicked() {
  if (auto item = listModels->currentItem()) {
    QString model_name = item->data(Qt::UserRole).toString();
    if (model_name.isEmpty())
      model_name = item->text();
    auto response = QMessageBox::question(
        this, tr("Confirm"),
        tr("Are you sure you want to remove model '%1'?").arg(model_name));
    if (response == QMessageBox::Yes) {
      btnDownloadModel->setEnabled(false);
      btnRemoveModel->setEnabled(false);
      textLog->append("Removing " + model_name + "...");
      cliProcess->start("vinput", QStringList()
                                      << "model" << "remove" << model_name);
    }
  }
}

void MainWindow::onDownloadModelClicked() {
  if (auto item = listRemoteModels->currentItem()) {
    QString model_name = item->data(Qt::UserRole).toString();
    if (model_name.isEmpty())
      model_name = item->text();
    btnDownloadModel->setEnabled(false);
    btnRemoveModel->setEnabled(false);
    textLog->append("Downloading " + model_name + "...");
    cliProcess->start("vinput", QStringList() << "model" << "add"
                                              << model_name);
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
  (void)exitCode;
  (void)exitStatus;
  textLog->append(tr("Process finished"));
  btnDownloadModel->setEnabled(true);
  btnRemoveModel->setEnabled(true);
  refreshModelList();
  loadConfigToUi();
}

void MainWindow::loadConfigToUi() {
  currentConfig = LoadCoreConfig();

  // Populate Device from CLI (PipeWire enumeration)
  comboDevice->clear();
  comboDevice->addItem("default", "default");

  QString device_err;
  QList<DeviceEntry> devices = LoadDevicesFromCli(&device_err);
  for (const auto &d : devices) {
    QString label = d.description.isEmpty()
                        ? d.name
                        : QString("%1 - %2").arg(d.name, d.description);
    comboDevice->addItem(label, d.name);
  }

  QString active_device = QString::fromStdString(currentConfig.captureDevice);
  if (active_device.isEmpty())
    active_device = "default";

  int active_index = -1;
  for (int i = 0; i < comboDevice->count(); ++i) {
    if (comboDevice->itemData(i).toString() == active_device) {
      active_index = i;
      break;
    }
  }
  if (active_index == -1 && active_device != "default") {
    comboDevice->addItem(active_device, active_device);
    active_index = comboDevice->count() - 1;
  }
  comboDevice->setCurrentIndex(active_index == -1 ? 0 : active_index);

  // Populate Models (Query from directory)
  comboModel->clear();
  QString model_err;
  QList<ModelEntry> local_models = LoadLocalModelsFromCli(&model_err);
  for (const auto &m : local_models) {
    comboModel->addItem(m.name);
  }
  if (!currentConfig.activeModel.empty()) {
    if (comboModel->findText(
            QString::fromStdString(currentConfig.activeModel)) == -1) {
      comboModel->addItem(QString::fromStdString(currentConfig.activeModel));
    }
    comboModel->setCurrentText(
        QString::fromStdString(currentConfig.activeModel));
  }

  // LLM
  checkLlmEnabled->setChecked(currentConfig.llm.enabled);
}

void MainWindow::onSaveClicked() {
  QString device_value = comboDevice->currentData().toString();
  if (device_value.isEmpty())
    device_value = comboDevice->currentText();
  currentConfig.captureDevice = device_value.toStdString();
  currentConfig.activeModel = comboModel->currentText().toStdString();
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

// ---------------------------------------------------------------------------
// Scene Tab
// ---------------------------------------------------------------------------

void MainWindow::setupSceneTab() {
  sceneTab = new QWidget();
  auto *layout = new QVBoxLayout(sceneTab);

  auto *listLayout = new QHBoxLayout();
  listScenes = new QListWidget();
  listLayout->addWidget(listScenes);

  auto *btnLayout = new QVBoxLayout();
  btnSceneAdd = new QPushButton(tr("Add"));
  btnSceneEdit = new QPushButton(tr("Edit"));
  btnSceneRemove = new QPushButton(tr("Remove"));
  btnSceneSetActive = new QPushButton(tr("Set Active"));
  btnLayout->addWidget(btnSceneAdd);
  btnLayout->addWidget(btnSceneEdit);
  btnLayout->addWidget(btnSceneRemove);
  btnLayout->addWidget(btnSceneSetActive);
  btnLayout->addStretch();
  listLayout->addLayout(btnLayout);

  layout->addLayout(listLayout);
  tabWidget->addTab(sceneTab, tr("Scene Management"));

  connect(btnSceneAdd, &QPushButton::clicked, this, &MainWindow::onSceneAdd);
  connect(btnSceneEdit, &QPushButton::clicked, this,
          &MainWindow::onSceneEdit);
  connect(btnSceneRemove, &QPushButton::clicked, this,
          &MainWindow::onSceneRemove);
  connect(btnSceneSetActive, &QPushButton::clicked, this,
          &MainWindow::onSceneSetActive);

  QTimer::singleShot(0, this, &MainWindow::refreshSceneList);
}

void MainWindow::refreshSceneList() {
  listScenes->clear();

  CoreConfig config = LoadCoreConfig();
  for (const auto &s : config.scenes.definitions) {
    QString label = QString::fromStdString(vinput::scene::DisplayLabel(s));
    bool active = (s.id == config.scenes.activeScene);

    QString display = label;
    if (active)
      display += " *";

    auto *item = new QListWidgetItem(display, listScenes);
    item->setData(Qt::UserRole, QString::fromStdString(s.id));
  }
}

void MainWindow::onSceneAdd() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Add Scene"));

  auto *form = new QFormLayout();
  auto *editId = new QLineEdit();
  auto *editLabel = new QLineEdit();
  auto *editPrompt = new QTextEdit();
  editPrompt->setMaximumHeight(100);

  form->addRow(tr("ID:"), editId);
  form->addRow(tr("Label:"), editLabel);
  form->addRow(tr("Prompt:"), editPrompt);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  CoreConfig config = LoadCoreConfig();

  vinput::scene::Definition def;
  def.id = editId->text().trimmed().toStdString();
  def.label = editLabel->text().trimmed().toStdString();
  def.prompt = editPrompt->toPlainText().toStdString();

  vinput::scene::Config sc;
  sc.activeSceneId = config.scenes.activeScene;
  sc.scenes = config.scenes.definitions;

  std::string err;
  if (!vinput::scene::AddScene(&sc, def, &err)) {
    QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
    return;
  }
  config.scenes.activeScene = sc.activeSceneId;
  config.scenes.definitions = sc.scenes;

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshSceneList();
}

void MainWindow::onSceneEdit() {
  auto *item = listScenes->currentItem();
  if (!item)
    return;

  QString scene_id = item->data(Qt::UserRole).toString();
  CoreConfig config = LoadCoreConfig();

  const vinput::scene::Definition *found = nullptr;
  for (const auto &s : config.scenes.definitions) {
    if (s.id == scene_id.toStdString()) {
      found = &s;
      break;
    }
  }
  if (!found)
    return;

  QDialog dialog(this);
  dialog.setWindowTitle(tr("Edit Scene"));

  auto *form = new QFormLayout();
  auto *editId = new QLineEdit(scene_id);
  editId->setReadOnly(true);
  auto *editLabel = new QLineEdit(QString::fromStdString(found->label));
  auto *editPrompt = new QTextEdit();
  editPrompt->setPlainText(QString::fromStdString(found->prompt));
  editPrompt->setMaximumHeight(100);

  form->addRow(tr("ID:"), editId);
  form->addRow(tr("Label:"), editLabel);
  form->addRow(tr("Prompt:"), editPrompt);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  vinput::scene::Definition updated;
  updated.label = editLabel->text().trimmed().toStdString();
  updated.prompt = editPrompt->toPlainText().toStdString();

  vinput::scene::Config sc;
  sc.activeSceneId = config.scenes.activeScene;
  sc.scenes = config.scenes.definitions;

  std::string err;
  if (!vinput::scene::UpdateScene(&sc, scene_id.toStdString(), updated,
                                  &err)) {
    QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
    return;
  }
  config.scenes.activeScene = sc.activeSceneId;
  config.scenes.definitions = sc.scenes;

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshSceneList();
}

void MainWindow::onSceneRemove() {
  auto *item = listScenes->currentItem();
  if (!item)
    return;

  QString scene_id = item->data(Qt::UserRole).toString();
  auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove scene '%1'?").arg(scene_id));
  if (response != QMessageBox::Yes)
    return;

  CoreConfig config = LoadCoreConfig();

  vinput::scene::Config sc;
  sc.activeSceneId = config.scenes.activeScene;
  sc.scenes = config.scenes.definitions;

  std::string err;
  if (!vinput::scene::RemoveScene(&sc, scene_id.toStdString(), true, &err)) {
    QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
    return;
  }
  config.scenes.activeScene = sc.activeSceneId;
  config.scenes.definitions = sc.scenes;

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshSceneList();
}

void MainWindow::onSceneSetActive() {
  auto *item = listScenes->currentItem();
  if (!item)
    return;

  QString scene_id = item->data(Qt::UserRole).toString();
  CoreConfig config = LoadCoreConfig();

  vinput::scene::Config sc;
  sc.activeSceneId = config.scenes.activeScene;
  sc.scenes = config.scenes.definitions;

  std::string err;
  if (!vinput::scene::SetActiveScene(&sc, scene_id.toStdString(), &err)) {
    QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
    return;
  }
  config.scenes.activeScene = sc.activeSceneId;
  config.scenes.definitions = sc.scenes;

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshSceneList();
}

// ---------------------------------------------------------------------------
// LLM Tab
// ---------------------------------------------------------------------------

void MainWindow::setupLlmTab() {
  llmTab = new QWidget();
  auto *layout = new QVBoxLayout(llmTab);

  auto *listLayout = new QHBoxLayout();
  listProviders = new QListWidget();
  listLayout->addWidget(listProviders);

  auto *btnLayout = new QVBoxLayout();
  btnLlmAdd = new QPushButton(tr("Add"));
  btnLlmEdit = new QPushButton(tr("Edit"));
  btnLlmRemove = new QPushButton(tr("Remove"));
  btnLlmSetActive = new QPushButton(tr("Set Active"));
  btnLayout->addWidget(btnLlmAdd);
  btnLayout->addWidget(btnLlmEdit);
  btnLayout->addWidget(btnLlmRemove);
  btnLayout->addWidget(btnLlmSetActive);
  btnLayout->addStretch();
  listLayout->addLayout(btnLayout);

  layout->addLayout(listLayout);
  tabWidget->addTab(llmTab, tr("LLM Providers"));

  connect(btnLlmAdd, &QPushButton::clicked, this, &MainWindow::onLlmAdd);
  connect(btnLlmEdit, &QPushButton::clicked, this, &MainWindow::onLlmEdit);
  connect(btnLlmRemove, &QPushButton::clicked, this,
          &MainWindow::onLlmRemove);
  connect(btnLlmSetActive, &QPushButton::clicked, this,
          &MainWindow::onLlmSetActive);

  QTimer::singleShot(0, this, &MainWindow::refreshLlmList);
}

void MainWindow::refreshLlmList() {
  listProviders->clear();

  CoreConfig config = LoadCoreConfig();
  for (const auto &p : config.llm.providers) {
    QString name = QString::fromStdString(p.name);
    QString base_url = QString::fromStdString(p.base_url);
    QString model = QString::fromStdString(p.model);
    bool active = (p.name == config.llm.activeProvider);

    QString display = QString("%1 - %2 @ %3").arg(name, model, base_url);
    if (active)
      display += " *";

    auto *item = new QListWidgetItem(display, listProviders);
    item->setData(Qt::UserRole, name);
  }
}

void MainWindow::onLlmAdd() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Add LLM Provider"));

  auto *form = new QFormLayout();
  auto *editName = new QLineEdit();
  auto *editBaseUrl = new QLineEdit();
  auto *editModel = new QLineEdit();
  auto *editApiKey = new QLineEdit();
  editApiKey->setEchoMode(QLineEdit::Password);

  form->addRow(tr("Name:"), editName);
  form->addRow(tr("Base URL:"), editBaseUrl);
  form->addRow(tr("Model:"), editModel);
  form->addRow(tr("API Key:"), editApiKey);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  CoreConfig config = LoadCoreConfig();

  std::string name = editName->text().trimmed().toStdString();
  for (const auto &p : config.llm.providers) {
    if (p.name == name) {
      QMessageBox::warning(
          this, tr("Error"),
          tr("Provider '%1' already exists.").arg(QString::fromStdString(name)));
      return;
    }
  }

  LlmProvider provider;
  provider.name = name;
  provider.base_url = editBaseUrl->text().trimmed().toStdString();
  provider.model = editModel->text().trimmed().toStdString();
  provider.api_key = editApiKey->text().toStdString();
  config.llm.providers.push_back(provider);

  if (config.llm.activeProvider.empty())
    config.llm.activeProvider = name;

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshLlmList();
}

void MainWindow::onLlmEdit() {
  auto *item = listProviders->currentItem();
  if (!item)
    return;

  QString provider_name = item->data(Qt::UserRole).toString();
  CoreConfig config = LoadCoreConfig();

  LlmProvider *found = nullptr;
  for (auto &p : config.llm.providers) {
    if (p.name == provider_name.toStdString()) {
      found = &p;
      break;
    }
  }
  if (!found)
    return;

  QDialog dialog(this);
  dialog.setWindowTitle(tr("Edit LLM Provider"));

  auto *form = new QFormLayout();
  auto *editName = new QLineEdit(provider_name);
  editName->setReadOnly(true);
  auto *editBaseUrl = new QLineEdit(QString::fromStdString(found->base_url));
  auto *editModel = new QLineEdit(QString::fromStdString(found->model));
  auto *editApiKey = new QLineEdit(QString::fromStdString(found->api_key));
  editApiKey->setEchoMode(QLineEdit::Password);

  form->addRow(tr("Name:"), editName);
  form->addRow(tr("Base URL:"), editBaseUrl);
  form->addRow(tr("Model:"), editModel);
  form->addRow(tr("API Key:"), editApiKey);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  found->base_url = editBaseUrl->text().trimmed().toStdString();
  found->model = editModel->text().trimmed().toStdString();
  found->api_key = editApiKey->text().toStdString();

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshLlmList();
}

void MainWindow::onLlmRemove() {
  auto *item = listProviders->currentItem();
  if (!item)
    return;

  QString provider_name = item->data(Qt::UserRole).toString();
  auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove LLM provider '%1'?")
          .arg(provider_name));
  if (response != QMessageBox::Yes)
    return;

  CoreConfig config = LoadCoreConfig();
  auto &providers = config.llm.providers;
  std::string name = provider_name.toStdString();

  auto it = std::find_if(providers.begin(), providers.end(),
                         [&name](const LlmProvider &p) {
                           return p.name == name;
                         });
  if (it == providers.end())
    return;

  providers.erase(it);
  if (config.llm.activeProvider == name)
    config.llm.activeProvider =
        providers.empty() ? "" : providers.front().name;

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshLlmList();
}

void MainWindow::onLlmSetActive() {
  auto *item = listProviders->currentItem();
  if (!item)
    return;

  QString provider_name = item->data(Qt::UserRole).toString();
  CoreConfig config = LoadCoreConfig();
  config.llm.activeProvider = provider_name.toStdString();

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshLlmList();
}

// ---------------------------------------------------------------------------
// Install Worker stubs (ModelInstallWorker not yet wired)
// ---------------------------------------------------------------------------

void MainWindow::onInstallProgress(quint64 downloaded, quint64 total,
                                   double speed) {
  (void)downloaded;
  (void)total;
  (void)speed;
}

void MainWindow::onInstallFinished(bool success, const QString &error) {
  (void)success;
  (void)error;
}
