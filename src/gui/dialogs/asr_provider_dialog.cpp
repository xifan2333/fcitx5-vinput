#include "dialogs/asr_provider_dialog.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtCore/qcoreapplication.h>
#include <QtWidgets/qmessagebox.h>

#include "config.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include "common/asr/model_manager.h"
#endif

#include "gui/utils/config_manager.h"

#include "utils/gui_helpers.h"

namespace vinput::gui {

namespace {

struct AsrProviderDialog {
  Q_DECLARE_TR_FUNCTIONS(AsrProviderDialog)
};

constexpr char kLocalType[] = "local";
constexpr char kCommandType[] = "command";

QString JoinArgLines(const std::vector<std::string>& args) {
  std::string joined;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i != 0)
      joined += "\n";
    joined += args[i];
  }
  return QString::fromStdString(joined);
}

QString JoinEnvLines(const std::map<std::string, std::string>& env) {
  std::string joined;
  std::size_t index = 0;
  for (const auto& [key, value] : env) {
    if (index++ != 0)
      joined += "\n";
    joined += key + "=" + value;
  }
  return QString::fromStdString(joined);
}

void UpdateFieldState(QComboBox* comboType, QComboBox* comboModel, QLineEdit* editCommand,
                      QTextEdit* textArgs, QTextEdit* textEnv, QSpinBox* spinTimeout) {
  const bool is_local = comboType->currentData().toString() == kLocalType;
  comboModel->setEnabled(is_local);
  editCommand->setEnabled(!is_local);
  textArgs->setEnabled(!is_local);
  textEnv->setEnabled(!is_local);
  spinTimeout->setEnabled(true);
}

QStringList LoadLocalModelIds() {
#if !VINPUT_ENABLE_LOCAL_ASR
  return {};
#else
  CoreConfig config = ConfigManager::Get().Load();
  ModelManager manager(ResolveModelBaseDir(config).string());
  auto models = manager.ListDetailed("");
  QStringList ids;
  for (const auto& m : models) {
    if (!m.id.empty()) {
      ids.push_back(QString::fromStdString(m.id));
    }
  }
  return ids;
#endif
}

} // namespace

bool ShowAsrProviderDialog(QWidget* parent, const QString& title, const AsrProviderData* existing,
                           AsrProviderData* out_data) {
  if (!out_data) {
    return false;
  }

  while (true) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* editName = new QLineEdit();
    auto* comboType = new QComboBox();
    auto* comboModel = new QComboBox();
    auto* editCommand = new QLineEdit();
    auto* textArgs = new QTextEdit();
    auto* textEnv = new QTextEdit();
    auto* spinTimeout = new QSpinBox();

#if VINPUT_ENABLE_LOCAL_ASR
    comboType->addItem(AsrProviderDialog::tr("local"), QString(kLocalType));
#endif
    comboType->addItem(AsrProviderDialog::tr("command"), QString(kCommandType));
    textArgs->setMaximumHeight(90);
    textEnv->setMaximumHeight(90);
    textArgs->setPlaceholderText(AsrProviderDialog::tr("One argument per line"));
    textEnv->setPlaceholderText(AsrProviderDialog::tr("One KEY=VALUE entry per line"));
    spinTimeout->setRange(1000, 300000);
    spinTimeout->setSingleStep(1000);
    spinTimeout->setSuffix(" ms");
    comboModel->setEditable(true);

    const QStringList local_models = LoadLocalModelIds();
    for (const auto& m : local_models) {
      comboModel->addItem(m);
    }

    AsrProviderData initial;
    if (existing) {
      initial = *existing;
    } else {
#if VINPUT_ENABLE_LOCAL_ASR
      initial.type = kLocalType;
#else
      initial.type = kCommandType;
#endif
      initial.timeout_ms = 15000;
    }

    editName->setText(QString::fromStdString(initial.id));
    editName->setReadOnly(existing != nullptr);
    comboType->setCurrentIndex(comboType->findData(QString::fromStdString(initial.type)));

    if (initial.type == kLocalType) {
      if (!initial.model.empty() &&
          comboModel->findText(QString::fromStdString(initial.model)) == -1) {
        comboModel->addItem(QString::fromStdString(initial.model));
      }
      comboModel->setCurrentText(QString::fromStdString(initial.model));
      editCommand->clear();
      textArgs->clear();
      textEnv->clear();
    } else {
      comboModel->setCurrentText(QString());
      editCommand->setText(QString::fromStdString(initial.command));
      textArgs->setPlainText(JoinArgLines(initial.args));
      textEnv->setPlainText(JoinEnvLines(initial.env));
    }
    spinTimeout->setValue(initial.timeout_ms > 0 ? initial.timeout_ms : spinTimeout->minimum());

    UpdateFieldState(comboType, comboModel, editCommand, textArgs, textEnv, spinTimeout);
    QObject::connect(comboType, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
                     [comboType, comboModel, editCommand, textArgs, textEnv, spinTimeout]() {
                       UpdateFieldState(comboType, comboModel, editCommand, textArgs, textEnv,
                                        spinTimeout);
                     });

    form->addRow(AsrProviderDialog::tr("Name:"), editName);
    form->addRow(AsrProviderDialog::tr("Type:"), comboType);
    form->addRow(AsrProviderDialog::tr("Model:"), comboModel);
    form->addRow(AsrProviderDialog::tr("Command / Interpreter:"), editCommand);
    form->addRow(AsrProviderDialog::tr("Args:"), textArgs);
    form->addRow(AsrProviderDialog::tr("Env:"), textEnv);
    form->addRow(AsrProviderDialog::tr("Timeout (ms):"), spinTimeout);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
      return false;
    }

    const QString name = editName->text().trimmed();
    const QString type = comboType->currentData().toString();
    if (name.isEmpty()) {
      QMessageBox::warning(parent, AsrProviderDialog::tr("Error"),
                           AsrProviderDialog::tr("Provider name must not be empty."));
      continue;
    }

    out_data->id = name.toStdString();
    out_data->type = type.toStdString();
    out_data->timeout_ms = spinTimeout->value();

#if VINPUT_ENABLE_LOCAL_ASR
    if (type == kLocalType) {
      out_data->model = comboModel->currentText().trimmed().toStdString();
      out_data->command.clear();
      out_data->args.clear();
      out_data->env.clear();
    } else
#endif
    {
      const QString command = editCommand->text().trimmed();
      if (command.isEmpty()) {
        QMessageBox::warning(parent, AsrProviderDialog::tr("Error"),
                             AsrProviderDialog::tr("Command providers require a command."));
        continue;
      }
      out_data->command = command.toStdString();
      out_data->model.clear();
      out_data->args.clear();
      for (const QString& arg : NonEmptyLines(textArgs->toPlainText())) {
        out_data->args.push_back(arg.toStdString());
      }
      QString env_error;
      if (!ParseCommandEnv(textEnv->toPlainText(), &out_data->env, &env_error)) {
        QMessageBox::warning(parent, AsrProviderDialog::tr("Error"), env_error);
        continue;
      }
    }
    return true;
  }
}

} // namespace vinput::gui
