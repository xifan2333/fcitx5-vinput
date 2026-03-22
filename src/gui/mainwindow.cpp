#include "mainwindow.h"
#include <QAbstractItemView>
#include <QColor>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <QApplication>
#include <QEventLoop>
#include <QPalette>
#include <QSettings>
#include <algorithm>

#include "common/asr_defaults.h"
#include "common/llm_defaults.h"
#include "common/path_utils.h"

namespace {

QString MainWindowTranslate(const char *sourceText) {
  return QCoreApplication::translate("MainWindow", sourceText);
}

bool ValidateProviderInput(const QString &name, const QString &base_url,
                           QString *error_out) {
  if (name.trimmed().isEmpty()) {
    if (error_out) {
      *error_out = MainWindowTranslate("Provider name must not be empty.");
    }
    return false;
  }

  const QUrl url = QUrl::fromUserInput(base_url.trimmed());
  if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty() ||
      (url.scheme() != "http" && url.scheme() != "https")) {
    if (error_out) {
      *error_out = MainWindowTranslate(
          "Base URL must be a valid http:// or https:// URL.");
    }
    return false;
  }

  return true;
}

QStringList NonEmptyLines(const QString &text) {
  QStringList lines;
  for (const QString &line : text.split('\n')) {
    const QString trimmed = line.trimmed();
    if (!trimmed.isEmpty()) {
      lines.push_back(trimmed);
    }
  }
  return lines;
}

QString AsrProviderDisplayText(const AsrProvider &provider, bool active) {
  QString display = QString::fromStdString(provider.name) + " [" +
                    QString::fromStdString(provider.type) + "]";
  if (provider.type == vinput::asr::kBuiltinProviderType) {
    display += " · " + QString::fromStdString(provider.model);
  } else if (!provider.command.empty()) {
    display += " · " + QString::fromStdString(provider.command);
  }
  if (active) {
    display += MainWindowTranslate(" *");
  }
  return display;
}

bool ParseAsrProviderEnv(const QString &text,
                         std::map<std::string, std::string> *env,
                         QString *error_out) {
  if (!env) {
    return false;
  }

  env->clear();
  for (const QString &line : NonEmptyLines(text)) {
    const int pos = line.indexOf('=');
    if (pos <= 0) {
      if (error_out) {
        *error_out = MainWindowTranslate("Invalid env entry '%1'. Use KEY=VALUE.")
                         .arg(line);
      }
      return false;
    }
    (*env)[line.left(pos).toStdString()] = line.mid(pos + 1).toStdString();
  }
  return true;
}

void UpdateAsrDialogFieldState(QComboBox *comboType, QLineEdit *editModel,
                               QLineEdit *editCommand, QTextEdit *textArgs,
                               QTextEdit *textEnv, QSpinBox *spinTimeout) {
  const bool is_builtin = comboType->currentData().toString() ==
                          QString::fromLatin1(vinput::asr::kBuiltinProviderType);
  editModel->setEnabled(is_builtin);
  editCommand->setEnabled(!is_builtin);
  textArgs->setEnabled(!is_builtin);
  textEnv->setEnabled(!is_builtin);
  spinTimeout->setEnabled(true);
}

bool EditAsrProviderDialog(QWidget *parent, const QString &title,
                           const CoreConfig &config,
                           const AsrProvider *existing_provider,
                           AsrProvider *out_provider) {
  if (!out_provider) {
    return false;
  }

  while (true) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);

    auto *form = new QFormLayout();
    auto *editName = new QLineEdit();
    auto *comboType = new QComboBox();
    auto *editModel = new QLineEdit();
    auto *editCommand = new QLineEdit();
    auto *textArgs = new QTextEdit();
    auto *textEnv = new QTextEdit();
    auto *spinTimeout = new QSpinBox();

    comboType->addItem(MainWindowTranslate("builtin"),
                       QString::fromLatin1(vinput::asr::kBuiltinProviderType));
    comboType->addItem(MainWindowTranslate("command"),
                       QString::fromLatin1(vinput::asr::kCommandProviderType));
    textArgs->setMaximumHeight(90);
    textEnv->setMaximumHeight(90);
    textArgs->setPlaceholderText(MainWindowTranslate("One argument per line"));
    textEnv->setPlaceholderText(
        MainWindowTranslate("One KEY=VALUE entry per line"));
    spinTimeout->setRange(1000, 300000);
    spinTimeout->setSingleStep(1000);
    spinTimeout->setSuffix(" ms");

    AsrProvider initial;
    if (existing_provider) {
      initial = *existing_provider;
    } else {
      initial.name = vinput::asr::kDefaultProviderName;
      initial.type = vinput::asr::kBuiltinProviderType;
      initial.model = vinput::asr::kDefaultBuiltinModel;
      initial.timeoutMs = vinput::asr::kDefaultProviderTimeoutMs;
    }

    editName->setText(QString::fromStdString(initial.name));
    editName->setReadOnly(existing_provider != nullptr);
    comboType->setCurrentIndex(
        comboType->findData(QString::fromStdString(initial.type)));
    editModel->setText(QString::fromStdString(initial.model));
    editCommand->setText(QString::fromStdString(initial.command));
    textArgs->setPlainText(
        QString::fromStdString([&]() {
          std::string joined;
          for (std::size_t i = 0; i < initial.args.size(); ++i) {
            if (i != 0) {
              joined += "\n";
            }
            joined += initial.args[i];
          }
          return joined;
        }()));
    textEnv->setPlainText(
        QString::fromStdString([&]() {
          std::string joined;
          std::size_t index = 0;
          for (const auto &[key, value] : initial.env) {
            if (index++ != 0) {
              joined += "\n";
            }
            joined += key + "=" + value;
          }
          return joined;
        }()));
    spinTimeout->setValue(initial.timeoutMs > 0
                              ? initial.timeoutMs
                              : vinput::asr::kDefaultProviderTimeoutMs);

    UpdateAsrDialogFieldState(comboType, editModel, editCommand, textArgs,
                              textEnv, spinTimeout);
    QObject::connect(comboType,
                     QOverload<int>::of(&QComboBox::currentIndexChanged),
                     &dialog,
                     [comboType, editModel, editCommand, textArgs, textEnv,
                      spinTimeout]() {
                       UpdateAsrDialogFieldState(comboType, editModel,
                                                 editCommand, textArgs, textEnv,
                                                 spinTimeout);
                     });

    form->addRow(MainWindowTranslate("Name:"), editName);
    form->addRow(MainWindowTranslate("Type:"), comboType);
    form->addRow(MainWindowTranslate("Model:"), editModel);
    form->addRow(MainWindowTranslate("Command:"), editCommand);
    form->addRow(MainWindowTranslate("Args:"), textArgs);
    form->addRow(MainWindowTranslate("Env:"), textEnv);
    form->addRow(MainWindowTranslate("Timeout (ms):"), spinTimeout);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
      return false;
    }

    const QString name = editName->text().trimmed();
    const QString type = comboType->currentData().toString();
    if (name.isEmpty()) {
      QMessageBox::warning(parent, MainWindowTranslate("Error"),
                           MainWindowTranslate("Provider name must not be empty."));
      continue;
    }

    if (!existing_provider &&
        ResolveAsrProvider(config, name.toStdString()) != nullptr) {
      QMessageBox::warning(parent, MainWindowTranslate("Error"),
                           MainWindowTranslate("Provider '%1' already exists.")
                               .arg(name));
      continue;
    }

    AsrProvider provider;
    provider.name = name.toStdString();
    provider.type = type.toStdString();
    provider.timeoutMs = spinTimeout->value();

    if (type == vinput::asr::kBuiltinProviderType) {
      provider.model = editModel->text().trimmed().isEmpty()
                           ? std::string(vinput::asr::kDefaultBuiltinModel)
                           : editModel->text().trimmed().toStdString();
    } else {
      const QString command = editCommand->text().trimmed();
      if (command.isEmpty()) {
        QMessageBox::warning(parent, MainWindowTranslate("Error"),
                             MainWindowTranslate("Command providers require a command."));
        continue;
      }
      provider.command = command.toStdString();
      for (const QString &arg : NonEmptyLines(textArgs->toPlainText())) {
        provider.args.push_back(arg.toStdString());
      }
      QString env_error;
      if (!ParseAsrProviderEnv(textEnv->toPlainText(), &provider.env,
                               &env_error)) {
        QMessageBox::warning(parent, MainWindowTranslate("Error"), env_error);
        continue;
      }
      provider.model.clear();
    }

    *out_provider = std::move(provider);
    return true;
  }
}

void RestartDaemonFromGui(QWidget *parent) {
  if (!QProcess::startDetached("vinput", QStringList() << "daemon"
                                                       << "restart")) {
    QMessageBox::warning(parent, MainWindowTranslate("Warning"),
                         MainWindowTranslate("Failed to restart daemon automatically."));
  }
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(tr("Vinput Configuration"));
  setupUi();
  loadConfigToUi();

  daemonRefreshTimer = new QTimer(this);
  connect(daemonRefreshTimer, &QTimer::timeout, this,
          &MainWindow::refreshDaemonStatus);
  daemonRefreshTimer->start(2000); // 2 seconds refresh rate
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
  setupAsrTab();
  setupHotwordTab();

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

  layout->addLayout(formLayout);

  layout->addSpacing(20);

  // Daemon Status Area
  auto *daemonFrame = new QFrame();
  daemonFrame->setFrameShape(QFrame::StyledPanel);
  auto *daemonLayout = new QVBoxLayout(daemonFrame);

  auto *statusLayout = new QHBoxLayout();
  statusLayout->addWidget(new QLabel(tr("Daemon Status:")));

  lblDaemonStatus = new QLabel(tr("Unknown"));
  QFont boldFont = lblDaemonStatus->font();
  boldFont.setBold(true);
  lblDaemonStatus->setFont(boldFont);
  statusLayout->addWidget(lblDaemonStatus);
  statusLayout->addStretch();

  daemonLayout->addLayout(statusLayout);

  auto *btnLayout = new QHBoxLayout();
  btnDaemonStart = new QPushButton(tr("Start Daemon"));
  btnDaemonStop = new QPushButton(tr("Stop Daemon"));
  btnDaemonRestart = new QPushButton(tr("Restart Daemon"));

  btnLayout->addWidget(btnDaemonStart);
  btnLayout->addWidget(btnDaemonStop);
  btnLayout->addWidget(btnDaemonRestart);
  btnLayout->addStretch();

  daemonLayout->addLayout(btnLayout);
  layout->addWidget(daemonFrame);

  connect(btnDaemonStart, &QPushButton::clicked, this,
          &MainWindow::onDaemonStart);
  connect(btnDaemonStop, &QPushButton::clicked, this,
          &MainWindow::onDaemonStop);
  connect(btnDaemonRestart, &QPushButton::clicked, this,
          &MainWindow::onDaemonRestart);

  // Initial trigger
  QTimer::singleShot(0, this, &MainWindow::refreshDaemonStatus);

  layout->addStretch();
  tabWidget->addTab(generalTab, tr("General Settings"));
}

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
  bool supports_hotwords = false;
};

bool RunVinputJson(const QStringList &args, QJsonDocument *out_doc,
                   QString *error_out) {
  QString vinput_path = QStandardPaths::findExecutable("vinput");
  if (vinput_path.isEmpty()) {
    if (error_out)
      *error_out = QObject::tr("vinput not found in PATH");
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
      *error_out = QObject::tr("vinput command timed out");
    return false;
  }

  QByteArray output = proc.readAllStandardOutput();

  if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
    // not only stderr, but also try to parse stdout
    if (out_doc && !output.isEmpty()) {
      QJsonParseError pe;
      QJsonDocument data = QJsonDocument::fromJson(output, &pe);
      if (pe.error == QJsonParseError::NoError)
        *out_doc = data;
    }
    if (error_out)
      *error_out = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    return false;
  }

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
  if (!RunVinputJson({"device", "list"}, &doc, error_out) || !doc.isArray()) {
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
  if (!RunVinputJson({"model", "list"}, &doc, error_out) || !doc.isArray()) {
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
    entry.supports_hotwords = obj.value("supports_hotwords").toBool(false);
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
    entry.supports_hotwords = obj.value("supports_hotwords").toBool(false);
    models.push_back(entry);
  }
  return models;
}

} // namespace

static void SetupTable(QTableWidget *t, const QStringList &headers) {
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

void MainWindow::setupModelTab() {
  modelTab = new QWidget();
  auto *layout = new QVBoxLayout(modelTab);

  // --- Installed Models ---
  auto *lblLocal = new QLabel(tr("<b>Installed Models</b>"));
  layout->addWidget(lblLocal);

  auto *topLayout = new QHBoxLayout();
  tableModels = new QTableWidget();
  SetupTable(tableModels, {tr("Name"), tr("Type"), tr("Language"), tr("Size"), tr("Hotwords"), tr("Status")});
  topLayout->addWidget(tableModels, 1);

  auto *btnLayout = new QVBoxLayout();
  btnUseModel = new QPushButton(tr("Use Selected"));
  btnRemoveModel = new QPushButton(tr("Remove Selected"));
  btnRefreshModels = new QPushButton(tr("Refresh"));
  btnLayout->addWidget(btnUseModel);
  btnLayout->addWidget(btnRemoveModel);
  btnLayout->addWidget(btnRefreshModels);
  btnLayout->addStretch();
  topLayout->addLayout(btnLayout);
  layout->addLayout(topLayout);

  // --- Remote Models ---
  auto *lblRemote = new QLabel(tr("<b>Remote Models (Registry)</b>"));
  layout->addWidget(lblRemote);

  auto *remoteLayout = new QHBoxLayout();
  tableRemoteModels = new QTableWidget();
  SetupTable(tableRemoteModels, {tr("Name"), tr("Display Name"), tr("Type"), tr("Language"), tr("Size"), tr("Hotwords"), tr("Status")});
  remoteLayout->addWidget(tableRemoteModels, 1);

  btnDownloadModel = new QPushButton(tr("Download Selected"));
  auto *dlLayout = new QVBoxLayout();
  dlLayout->addWidget(btnDownloadModel);
  dlLayout->addStretch();
  remoteLayout->addLayout(dlLayout);
  layout->addLayout(remoteLayout);

  // --- Log Area ---
  textLog = new QTextEdit();
  textLog->setReadOnly(true);
  textLog->setMaximumHeight(100);
  layout->addWidget(textLog);

  tabWidget->addTab(modelTab, tr("Model Management"));

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

  QTimer::singleShot(0, this, &MainWindow::refreshModelList);
}

static QTableWidgetItem *MakeCell(const QString &text, const QString &data = {}) {
  auto *cell = new QTableWidgetItem(text);
  cell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
  if (!data.isEmpty())
    cell->setData(Qt::UserRole, data);
  return cell;
}

void MainWindow::refreshModelList() {
  tableModels->setRowCount(0);

  const QPalette pal = QApplication::palette();
  const QColor colorPositive = pal.color(QPalette::Active, QPalette::Link);
  const QColor colorDisabled = pal.color(QPalette::Disabled, QPalette::Text);
  const QColor colorHighlight = pal.color(QPalette::Active, QPalette::Highlight);
  const QColor colorError = QColor(198, 40, 40); // red — universal error color

  QString local_err;
  QList<ModelEntry> local_models = LoadLocalModelsFromCli(&local_err);
  if (!local_err.isEmpty())
    textLog->append(tr("Local model list error: %1").arg(local_err));

  for (const auto &m : local_models) {
    int row = tableModels->rowCount();
    tableModels->insertRow(row);
    tableModels->setItem(row, 0, MakeCell(m.name, m.name));
    tableModels->setItem(row, 1, MakeCell(m.model_type));
    tableModels->setItem(row, 2, MakeCell(m.language));
    auto *sizeCell = MakeCell(m.size);
    sizeCell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tableModels->setItem(row, 3, sizeCell);
    QString hw = m.supports_hotwords ? tr("yes") : tr("no");
    auto *hwCell = MakeCell(hw);
    hwCell->setForeground(m.supports_hotwords ? colorPositive : colorDisabled);
    tableModels->setItem(row, 4, hwCell);
    QString status = m.status.isEmpty() ? tr("installed") : m.status;
    if (m.status == "active") status = tr("active");
    else if (m.status == "broken") status = tr("broken");
    else if (m.status == "installed") status = tr("installed");
    auto *stCell = MakeCell(status);
    if (m.status == "active") {
      stCell->setForeground(colorHighlight);
      QFont f = stCell->font(); f.setBold(true); stCell->setFont(f);
    } else if (m.status == "broken") {
      stCell->setForeground(colorError);
    }
    tableModels->setItem(row, 5, stCell);
  }

  tableRemoteModels->setRowCount(0);
  QString remote_err;
  QList<ModelEntry> remote_models = LoadRemoteModelsFromCli(&remote_err);
  if (!remote_err.isEmpty())
    textLog->append(tr("Remote model list error: %1").arg(remote_err));

  for (const auto &m : remote_models) {
    int row = tableRemoteModels->rowCount();
    tableRemoteModels->insertRow(row);
    tableRemoteModels->setItem(row, 0, MakeCell(m.name, m.name));
    tableRemoteModels->setItem(row, 1, MakeCell(m.display_name));
    tableRemoteModels->setItem(row, 2, MakeCell(m.model_type));
    tableRemoteModels->setItem(row, 3, MakeCell(m.language));
    auto *remoteSizeCell = MakeCell(m.size);
    remoteSizeCell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tableRemoteModels->setItem(row, 4, remoteSizeCell);
    QString hw = m.supports_hotwords ? tr("yes") : tr("no");
    auto *hwCell = MakeCell(hw);
    hwCell->setForeground(m.supports_hotwords ? colorPositive : colorDisabled);
    tableRemoteModels->setItem(row, 5, hwCell);
    QString remoteStatus = m.status;
    if (m.status == "installed") remoteStatus = tr("installed");
    else if (m.status == "available") remoteStatus = tr("available");
    auto *stCell = MakeCell(remoteStatus);
    if (m.status == "installed") {
      stCell->setForeground(colorDisabled);
      for (int c = 0; c < tableRemoteModels->columnCount(); ++c) {
        if (auto *ci = tableRemoteModels->item(row, c))
          ci->setFlags(ci->flags() & ~Qt::ItemIsEnabled);
      }
    } else {
      stCell->setForeground(colorPositive);
    }
    tableRemoteModels->setItem(row, 6, stCell);
  }
}

void MainWindow::onUseModelClicked() {
  auto items = tableModels->selectedItems();
  if (items.isEmpty()) return;
  QString model_name = tableModels->item(tableModels->currentRow(), 0)
                           ->data(Qt::UserRole).toString();
  if (model_name.isEmpty())
    model_name = tableModels->item(tableModels->currentRow(), 0)->text();
  comboModel->setCurrentText(model_name);
  tabWidget->setCurrentWidget(generalTab);
}

void MainWindow::onRemoveModelClicked() {
  auto items = tableModels->selectedItems();
  if (items.isEmpty()) return;
  QString model_name = tableModels->item(tableModels->currentRow(), 0)
                           ->data(Qt::UserRole).toString();
  if (model_name.isEmpty())
    model_name = tableModels->item(tableModels->currentRow(), 0)->text();
  auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove model '%1'?").arg(model_name));
  if (response == QMessageBox::Yes) {
    btnDownloadModel->setEnabled(false);
    btnRemoveModel->setEnabled(false);
    textLog->append(tr("Removing %1...").arg(model_name));
    cliProcess->start("vinput", QStringList()
                                    << "model" << "remove" << "--force" << model_name);
  }
}

void MainWindow::onDownloadModelClicked() {
  auto items = tableRemoteModels->selectedItems();
  if (items.isEmpty()) return;
  QString model_name = tableRemoteModels->item(tableRemoteModels->currentRow(), 0)
                           ->data(Qt::UserRole).toString();
  if (model_name.isEmpty())
    model_name = tableRemoteModels->item(tableRemoteModels->currentRow(), 0)->text();
  btnDownloadModel->setEnabled(false);
  btnRemoveModel->setEnabled(false);
  cliProcess->start("vinput", QStringList()
                                  << "model" << "add" << model_name);
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
  const std::string active_model = ResolvePreferredBuiltinModel(currentConfig);
  if (!active_model.empty()) {
    if (comboModel->findText(
            QString::fromStdString(active_model)) == -1) {
      comboModel->addItem(QString::fromStdString(active_model));
    }
    comboModel->setCurrentText(QString::fromStdString(active_model));
  }

  // Hotwords
  editHotwordsFile->setText(QString::fromStdString(currentConfig.hotwordsFile));
  if (!currentConfig.hotwordsFile.empty()) {
    QFile f(QString::fromStdString(currentConfig.hotwordsFile));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
      textHotwords->setPlainText(QTextStream(&f).readAll());
    }
  } else {
    textHotwords->clear();
  }
}

void MainWindow::onSaveClicked() {
  // Reload config to avoid overwriting changes from other tabs if they aren't
  // bound strictly
  currentConfig = LoadCoreConfig();

  QString device_value = comboDevice->currentData().toString();
  if (device_value.isEmpty())
    device_value = comboDevice->currentText();
  currentConfig.captureDevice = device_value.toStdString();
  std::string model_error;
  if (!SetPreferredBuiltinModel(&currentConfig,
                                comboModel->currentText().toStdString(),
                                &model_error)) {
    QMessageBox::critical(this, tr("Error"),
                          QString::fromStdString(model_error));
    return;
  }

  currentConfig.hotwordsFile = editHotwordsFile->text().trimmed().toStdString();
  if (!currentConfig.hotwordsFile.empty()) {
    QFile f(QString::fromStdString(currentConfig.hotwordsFile));
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
      QTextStream(&f) << textHotwords->toPlainText();
    }
  }

  if (SaveCoreConfig(currentConfig)) {
    // notify fcitx5 daemon to load new config via dbus
    QProcess::startDetached("vinput", QStringList() << "daemon" << "restart");
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
  connect(btnSceneEdit, &QPushButton::clicked, this, &MainWindow::onSceneEdit);
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

QString ProviderModelCacheKey(const LlmProvider &provider) {
  return QString::fromStdString(provider.base_url) + "\n" +
         QString::fromStdString(provider.api_key);
}

void ApplyFetchedProviderModels(QComboBox *comboModel,
                                const QStringList &models) {
  comboModel->setEnabled(true);
  comboModel->clear();
  comboModel->setToolTip(QString());
  if (auto *lineEdit = comboModel->lineEdit()) {
        lineEdit->setPlaceholderText(
        models.isEmpty()
            ? MainWindowTranslate("No models returned. You can type one manually.")
            : QString());
  }
  comboModel->addItems(models);

  const QString desiredModel =
      comboModel->property("vinput_desired_model").toString();
  if (!desiredModel.isEmpty()) {
    comboModel->setCurrentText(desiredModel);
  }
  comboModel->setProperty("vinput_desired_model", QString());
}

void ApplyProviderModelFetchError(QComboBox *comboModel,
                                  const QString &error) {
  comboModel->setEnabled(true);
  comboModel->clear();
  comboModel->setToolTip(error);
  if (auto *lineEdit = comboModel->lineEdit()) {
    lineEdit->setPlaceholderText(
        MainWindowTranslate("Failed to load models. Type one manually or reselect the "
                            "provider to retry."));
  }

  const QString desiredModel =
      comboModel->property("vinput_desired_model").toString();
  if (!desiredModel.isEmpty()) {
    comboModel->setCurrentText(desiredModel);
  }
  comboModel->setProperty("vinput_desired_model", QString());
}

void FetchModelsFromProviderAsync(const LlmProvider &provider,
                                  QComboBox *comboModel) {
  static QHash<QString, QStringList> cache;

  if (provider.base_url.empty()) {
    comboModel->setEnabled(true);
    comboModel->clear();
    comboModel->setToolTip(QString());
    if (auto *lineEdit = comboModel->lineEdit()) {
      lineEdit->setPlaceholderText(QString());
    }
    return;
  }

  const QString cacheKey = ProviderModelCacheKey(provider);
  const int generation =
      comboModel->property("vinput_provider_fetch_generation").toInt() + 1;
  comboModel->setProperty("vinput_provider_fetch_generation", generation);
  comboModel->setProperty("vinput_provider_fetch_key", cacheKey);

  if (cache.contains(cacheKey)) {
    ApplyFetchedProviderModels(comboModel, cache.value(cacheKey));
    return;
  }

  comboModel->setEnabled(false);
  comboModel->clear();
  comboModel->setToolTip(QString());
  if (auto *lineEdit = comboModel->lineEdit()) {
    lineEdit->setPlaceholderText(MainWindowTranslate("Loading models..."));
  }

  QString url = QString::fromStdString(provider.base_url);
  if (!url.endsWith('/'))
    url += '/';
  url += vinput::llm::kOpenAiModelsPath;

  auto *nam = new QNetworkAccessManager(comboModel);
  QNetworkRequest req{QUrl(url)};
  req.setRawHeader(vinput::llm::kAuthorizationHeader,
                   QByteArray(vinput::llm::kBearerPrefix) +
                       QByteArray::fromStdString(provider.api_key));

  QNetworkReply *reply = nam->get(req);
  auto *timeout = new QTimer(reply);
  timeout->setSingleShot(true);
  QObject::connect(timeout, &QTimer::timeout, reply, [reply]() {
    if (!reply->isFinished()) {
      reply->abort();
    }
  });
  timeout->start(vinput::llm::kModelFetchTimeoutMs);

  QObject::connect(reply, &QNetworkReply::finished, comboModel,
                   [comboModel, reply, timeout, cacheKey, generation]() {
                     timeout->stop();

                     const bool stale =
                         comboModel->property("vinput_provider_fetch_generation")
                             .toInt() != generation ||
                         comboModel->property("vinput_provider_fetch_key")
                             .toString() != cacheKey;

                     QStringList models;
                     QString error;
                     bool success = false;
                     if (reply->error() == QNetworkReply::NoError) {
                       QJsonParseError parseError;
                       const QJsonDocument doc =
                           QJsonDocument::fromJson(reply->readAll(),
                                                   &parseError);
                       if (parseError.error != QJsonParseError::NoError ||
                           !doc.isObject() ||
                           !doc.object().value("data").isArray()) {
                         error = MainWindowTranslate(
                             "Provider returned invalid JSON for /v1/models.");
                       } else {
                         const QJsonArray data =
                             doc.object().value("data").toArray();
                         for (const auto &v : data) {
                           const QString id =
                               v.toObject().value("id").toString();
                           if (!id.isEmpty()) {
                             models.append(id);
                           }
                         }
                         models.removeDuplicates();
                         models.sort();
                         success = true;
                       }
                     } else {
                       error = reply->errorString();
                     }

                     if (!stale) {
                       if (success) {
                         cache.insert(cacheKey, models);
                         ApplyFetchedProviderModels(comboModel, models);
                       } else {
                         ApplyProviderModelFetchError(comboModel, error);
                       }
                     }

                     reply->deleteLater();
                     reply->manager()->deleteLater();
                   });
}

// Populate provider combo from config, wire it to refresh model combo.
static void SetupProviderModelCombos(QComboBox *comboProvider,
                                     QComboBox *comboModel,
                                     const CoreConfig &config,
                                     const QString &currentProvider = {},
                                     const QString &currentModel = {}) {
  comboProvider->clear();
  comboProvider->addItem(QString());  // empty = inherit / none
  for (const auto &p : config.llm.providers)
    comboProvider->addItem(QString::fromStdString(p.name));

  comboModel->setEditable(true);
  comboModel->setProperty("vinput_desired_model", currentModel);

  // Copy providers so the lambda is self-contained.
  auto providers = config.llm.providers;
  auto refreshModels = [comboModel, comboProvider, providers]() {
    QString selected = comboProvider->currentText();
    if (selected.isEmpty()) {
      comboModel->setEnabled(true);
      comboModel->clear();
      comboModel->setToolTip(QString());
      if (auto *lineEdit = comboModel->lineEdit()) {
        lineEdit->setPlaceholderText(QString());
      }
      return;
    }
    for (const auto &p : providers) {
      if (p.name == selected.toStdString()) {
        FetchModelsFromProviderAsync(p, comboModel);
        break;
      }
    }
  };

  QObject::connect(comboProvider, QOverload<int>::of(&QComboBox::activated),
                   comboProvider, refreshModels);

  if (!currentProvider.isEmpty()) {
    comboProvider->setCurrentText(currentProvider);
    refreshModels();
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
  auto *comboProvider = new QComboBox();
  auto *comboModel = new QComboBox();
  auto *spinTimeout = new QSpinBox();
  spinTimeout->setRange(1000, 300000);
  spinTimeout->setSingleStep(1000);
  spinTimeout->setValue(vinput::scene::kDefaultTimeoutMs);
  spinTimeout->setSuffix(" ms");
  auto *spinCandidates = new QSpinBox();
  spinCandidates->setRange(vinput::scene::kMinCandidateCount,
                           vinput::scene::kMaxCandidateCount);
  spinCandidates->setValue(vinput::scene::kDefaultCandidateCount);

  CoreConfig config = LoadCoreConfig();
  SetupProviderModelCombos(comboProvider, comboModel, config);

  form->addRow(tr("ID:"), editId);
  form->addRow(tr("Label:"), editLabel);
  form->addRow(tr("Prompt:"), editPrompt);
  form->addRow(tr("Provider:"), comboProvider);
  form->addRow(tr("Model:"), comboModel);
  form->addRow(tr("Candidate Count:"), spinCandidates);
  form->addRow(tr("Timeout (ms):"), spinTimeout);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  vinput::scene::Definition def;
  def.id = editId->text().trimmed().toStdString();
  def.label = editLabel->text().trimmed().toStdString();
  def.prompt = editPrompt->toPlainText().toStdString();
  def.provider_id = comboProvider->currentText().toStdString();
  def.model = comboModel->currentText().trimmed().toStdString();
  def.candidate_count = spinCandidates->value();
  def.timeout_ms = spinTimeout->value();

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
  auto *comboProvider = new QComboBox();
  auto *comboModel = new QComboBox();
  auto *spinTimeout = new QSpinBox();
  spinTimeout->setRange(1000, 300000);
  spinTimeout->setSingleStep(1000);
  spinTimeout->setValue(found->timeout_ms);
  spinTimeout->setSuffix(" ms");
  auto *spinCandidates = new QSpinBox();
  spinCandidates->setRange(vinput::scene::kMinCandidateCount,
                           vinput::scene::kMaxCandidateCount);
  spinCandidates->setValue(found->candidate_count);

  SetupProviderModelCombos(comboProvider, comboModel, config,
                           QString::fromStdString(found->provider_id),
                           QString::fromStdString(found->model));

  form->addRow(tr("ID:"), editId);
  form->addRow(tr("Label:"), editLabel);
  form->addRow(tr("Prompt:"), editPrompt);
  form->addRow(tr("Provider:"), comboProvider);
  form->addRow(tr("Model:"), comboModel);
  form->addRow(tr("Candidate Count:"), spinCandidates);
  form->addRow(tr("Timeout (ms):"), spinTimeout);

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
  updated.provider_id = comboProvider->currentText().toStdString();
  updated.model = comboModel->currentText().trimmed().toStdString();
  updated.candidate_count = spinCandidates->value();
  updated.timeout_ms = spinTimeout->value();

  vinput::scene::Config sc;
  sc.activeSceneId = config.scenes.activeScene;
  sc.scenes = config.scenes.definitions;

  std::string err;
  if (!vinput::scene::UpdateScene(&sc, scene_id.toStdString(), updated, &err)) {
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
  btnLayout->addWidget(btnLlmAdd);
  btnLayout->addWidget(btnLlmEdit);
  btnLayout->addWidget(btnLlmRemove);
  btnLayout->addStretch();
  listLayout->addLayout(btnLayout);

  layout->addLayout(listLayout);
  tabWidget->addTab(llmTab, tr("LLM Providers"));

  connect(btnLlmAdd, &QPushButton::clicked, this, &MainWindow::onLlmAdd);
  connect(btnLlmEdit, &QPushButton::clicked, this, &MainWindow::onLlmEdit);
  connect(btnLlmRemove, &QPushButton::clicked, this, &MainWindow::onLlmRemove);

  QTimer::singleShot(0, this, &MainWindow::refreshLlmList);
}

void MainWindow::refreshLlmList() {
  listProviders->clear();

  CoreConfig config = LoadCoreConfig();
  for (const auto &p : config.llm.providers) {
    QString name = QString::fromStdString(p.name);
    QString base_url = QString::fromStdString(p.base_url);
    QString display = QString("%1 @ %2").arg(name, base_url);

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
  auto *editApiKey = new QLineEdit();
  editApiKey->setEchoMode(QLineEdit::Password);

  form->addRow(tr("Name:"), editName);
  form->addRow(tr("Base URL:"), editBaseUrl);
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

  const QString name_text = editName->text().trimmed();
  const QString base_url_text = editBaseUrl->text().trimmed();
  QString validation_error;
  if (!ValidateProviderInput(name_text, base_url_text, &validation_error)) {
    QMessageBox::warning(this, tr("Error"), validation_error);
    return;
  }

  std::string name = name_text.toStdString();
  for (const auto &p : config.llm.providers) {
    if (p.name == name) {
      QMessageBox::warning(this, tr("Error"),
                           tr("Provider '%1' already exists.")
                               .arg(QString::fromStdString(name)));
      return;
    }
  }

  LlmProvider provider;
  provider.name = name;
  provider.base_url = base_url_text.toStdString();
  provider.api_key = editApiKey->text().toStdString();
  config.llm.providers.push_back(provider);

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
  auto *editApiKey = new QLineEdit(QString::fromStdString(found->api_key));
  editApiKey->setEchoMode(QLineEdit::Password);

  form->addRow(tr("Name:"), editName);
  form->addRow(tr("Base URL:"), editBaseUrl);
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

  const QString base_url_text = editBaseUrl->text().trimmed();
  QString validation_error;
  if (!ValidateProviderInput(provider_name, base_url_text, &validation_error)) {
    QMessageBox::warning(this, tr("Error"), validation_error);
    return;
  }

  found->base_url = base_url_text.toStdString();
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

  auto it =
      std::find_if(providers.begin(), providers.end(),
                   [&name](const LlmProvider &p) { return p.name == name; });
  if (it == providers.end())
    return;

  providers.erase(it);

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }
  refreshLlmList();
}

// ---------------------------------------------------------------------------
// ASR Tab
// ---------------------------------------------------------------------------

void MainWindow::setupAsrTab() {
  asrTab = new QWidget();
  auto *layout = new QVBoxLayout(asrTab);

  auto *listLayout = new QHBoxLayout();
  listAsrProviders = new QListWidget();
  listLayout->addWidget(listAsrProviders);

  auto *btnLayout = new QVBoxLayout();
  btnAsrAdd = new QPushButton(tr("Add"));
  btnAsrEdit = new QPushButton(tr("Edit"));
  btnAsrRemove = new QPushButton(tr("Remove"));
  btnAsrSetActive = new QPushButton(tr("Set Active"));
  btnLayout->addWidget(btnAsrAdd);
  btnLayout->addWidget(btnAsrEdit);
  btnLayout->addWidget(btnAsrRemove);
  btnLayout->addWidget(btnAsrSetActive);
  btnLayout->addStretch();
  listLayout->addLayout(btnLayout);

  layout->addLayout(listLayout);
  tabWidget->addTab(asrTab, tr("ASR Providers"));

  connect(btnAsrAdd, &QPushButton::clicked, this, &MainWindow::onAsrAdd);
  connect(btnAsrEdit, &QPushButton::clicked, this, &MainWindow::onAsrEdit);
  connect(btnAsrRemove, &QPushButton::clicked, this, &MainWindow::onAsrRemove);
  connect(btnAsrSetActive, &QPushButton::clicked, this,
          &MainWindow::onAsrSetActive);

  QTimer::singleShot(0, this, &MainWindow::refreshAsrList);
}

void MainWindow::refreshAsrList() {
  listAsrProviders->clear();

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);
  for (const auto &provider : config.asr.providers) {
    const bool active = provider.name == config.asr.activeProvider;
    auto *item = new QListWidgetItem(
        AsrProviderDisplayText(provider, active), listAsrProviders);
    item->setData(Qt::UserRole, QString::fromStdString(provider.name));
  }
}

void MainWindow::onAsrAdd() {
  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  AsrProvider provider;
  if (!EditAsrProviderDialog(this, tr("Add ASR Provider"), config, nullptr,
                             &provider)) {
    return;
  }

  config.asr.providers.push_back(std::move(provider));
  NormalizeCoreConfig(&config);

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }

  RestartDaemonFromGui(this);
  refreshAsrList();
  loadConfigToUi();
}

void MainWindow::onAsrEdit() {
  auto *item = listAsrProviders->currentItem();
  if (!item) {
    return;
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const QString provider_name = item->data(Qt::UserRole).toString();
  AsrProvider *found = nullptr;
  for (auto &provider : config.asr.providers) {
    if (provider.name == provider_name.toStdString()) {
      found = &provider;
      break;
    }
  }
  if (!found) {
    return;
  }

  AsrProvider updated;
  if (!EditAsrProviderDialog(this, tr("Edit ASR Provider"), config, found,
                             &updated)) {
    return;
  }

  *found = std::move(updated);
  NormalizeCoreConfig(&config);

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }

  RestartDaemonFromGui(this);
  refreshAsrList();
  loadConfigToUi();
}

void MainWindow::onAsrRemove() {
  auto *item = listAsrProviders->currentItem();
  if (!item) {
    return;
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const QString provider_name = item->data(Qt::UserRole).toString();
  auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove ASR provider '%1'?")
          .arg(provider_name));
  if (response != QMessageBox::Yes) {
    return;
  }

  auto &providers = config.asr.providers;
  const std::string name = provider_name.toStdString();
  auto it = std::find_if(providers.begin(), providers.end(),
                         [&name](const AsrProvider &provider) {
                           return provider.name == name;
                         });
  if (it == providers.end()) {
    return;
  }

  const bool removing_active = config.asr.activeProvider == name;
  providers.erase(it);
  if (removing_active) {
    config.asr.activeProvider.clear();
  }
  NormalizeCoreConfig(&config);

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }

  RestartDaemonFromGui(this);
  refreshAsrList();
  loadConfigToUi();
}

void MainWindow::onAsrSetActive() {
  auto *item = listAsrProviders->currentItem();
  if (!item) {
    return;
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);
  config.asr.activeProvider = item->data(Qt::UserRole).toString().toStdString();
  NormalizeCoreConfig(&config);

  if (!SaveCoreConfig(config)) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save configuration."));
    return;
  }

  RestartDaemonFromGui(this);
  refreshAsrList();
  loadConfigToUi();
}

// ---------------------------------------------------------------------------
// Hotword Tab
// ---------------------------------------------------------------------------

void MainWindow::setupHotwordTab() {
  hotwordTab = new QWidget();
  auto *layout = new QVBoxLayout(hotwordTab);

  auto *fileLayout = new QHBoxLayout();
  editHotwordsFile = new QLineEdit();
  editHotwordsFile->setPlaceholderText(tr("Path to hotwords file..."));
  btnBrowseHotwords = new QPushButton(tr("Browse..."));
  fileLayout->addWidget(editHotwordsFile);
  fileLayout->addWidget(btnBrowseHotwords);
  layout->addLayout(fileLayout);

  auto *lblWords = new QLabel(tr("Hotwords (one per line, optional per-word score: \"word 2.0\"):" ));
  layout->addWidget(lblWords);

  textHotwords = new QTextEdit();
  layout->addWidget(textHotwords);

  tabWidget->addTab(hotwordTab, tr("Hotword Settings"));

  connect(btnBrowseHotwords, &QPushButton::clicked, this,
          &MainWindow::onBrowseHotwordsClicked);
  connect(editHotwordsFile, &QLineEdit::editingFinished, this, [this]() {
    QString path = editHotwordsFile->text().trimmed();
    if (path.isEmpty()) { textHotwords->clear(); return; }
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
      textHotwords->setPlainText(QTextStream(&f).readAll());
  });
}

void MainWindow::onBrowseHotwordsClicked() {
  QString fileName = QFileDialog::getOpenFileName(
      this, tr("Select Hotwords File"), "", tr("Text Files (*.txt);;All Files (*)"));
  if (fileName.isEmpty()) return;
  editHotwordsFile->setText(fileName);
  QFile f(fileName);
  if (f.open(QIODevice::ReadOnly | QIODevice::Text))
    textHotwords->setPlainText(QTextStream(&f).readAll());
}

// ---------------------------------------------------------------------------
// Daemon Control
// ---------------------------------------------------------------------------

void MainWindow::refreshDaemonStatus() {
  QJsonDocument doc;
  QString err;
  bool ok = RunVinputJson({"status"}, &doc, &err);

  // if daemon is not running, vinput status return code may not be 0
  // but we can still show "Stopped" instead of "Error:"
  if (!ok && doc.isObject())
    ok = true;

  if (!ok || !doc.isObject()) {
    lblDaemonStatus->setText(tr("Error: %1").arg(err));
    lblDaemonStatus->setStyleSheet("color: red;");
    btnDaemonStart->setEnabled(true);
    btnDaemonStop->setEnabled(false);
    btnDaemonRestart->setEnabled(false);
    return;
  }

  QJsonObject obj = doc.object();
  bool running = obj.value("running").toBool();

  if (!running) {
    lblDaemonStatus->setText(tr("Stopped"));
    lblDaemonStatus->setStyleSheet("color: gray;");
    btnDaemonStart->setEnabled(true);
    btnDaemonStop->setEnabled(false);
    btnDaemonRestart->setEnabled(false);
    return;
  }

  QString status = obj.value("status").toString();
  if (status.isEmpty()) {
    QString runtime_err = obj.value("error").toString();
    lblDaemonStatus->setText(tr("Running (Status Error: %1)").arg(runtime_err));
    lblDaemonStatus->setStyleSheet("color: orange;");
  } else {
    lblDaemonStatus->setText(tr("Running: %1").arg(status));
    lblDaemonStatus->setStyleSheet("color: green;");
  }

  btnDaemonStart->setEnabled(false);
  btnDaemonStop->setEnabled(true);
  btnDaemonRestart->setEnabled(true);
}

void MainWindow::onDaemonStart() {
  btnDaemonStart->setEnabled(false);
  QProcess::startDetached("vinput", QStringList() << "daemon" << "start");
  // The timer will catch the state change shortly.

  // Try to show a message box with error logs
  QTimer::singleShot(1500, this, [this]() {
    QJsonDocument doc;
    QString err;
    RunVinputJson({"status"}, &doc, &err);
    if (doc.isObject() && !doc.object().value("running").toBool()) {
      // daemon start failed, try to get logs
      QProcess logProc;
      logProc.start("vinput", QStringList() << "daemon" << "logs" << "-n" << "5");
      logProc.waitForFinished(3000);
      QString logs = QString::fromUtf8(logProc.readAllStandardOutput() +
                                       logProc.readAllStandardError()).trimmed();
      if (!logs.isEmpty()) {
        QMessageBox::warning(this, tr("Error"),
                             logs);
      }
    }
    refreshDaemonStatus();
  });
}

void MainWindow::onDaemonStop() {
  btnDaemonStop->setEnabled(false);
  QProcess::startDetached("vinput", QStringList() << "daemon" << "stop");
  // The timer will catch the state change shortly.
}

void MainWindow::onDaemonRestart() {
  btnDaemonRestart->setEnabled(false);
  btnDaemonStop->setEnabled(false);
  QProcess::startDetached("vinput", QStringList() << "daemon" << "restart");
  // The timer will catch the state change shortly.
}
