#include "dialogs/asr_provider_dialog.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QtCore/qcoreapplication.h>

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

void UpdateFieldState(QComboBox* comboType, QComboBox* comboModel, CommandEditorGroup* cmd_editor,
                      QSpinBox* spinTimeout) {
  const bool is_local = comboType->currentData().toString() == kLocalType;
  comboModel->setEnabled(is_local);
  if (cmd_editor != nullptr) {
    cmd_editor->SetEnabled(!is_local);
  }
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
  if (out_data == nullptr) {
    return false;
  }

  while (true) {
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* editName = new QLineEdit();
    auto* comboType = new QComboBox();
    auto* comboModel = new QComboBox();
    auto* spinTimeout = new QSpinBox();

#if VINPUT_ENABLE_LOCAL_ASR
    comboType->addItem(AsrProviderDialog::tr("local"), QString(kLocalType));
#endif
    comboType->addItem(AsrProviderDialog::tr("command"), QString(kCommandType));
    spinTimeout->setRange(1000, 300000);
    spinTimeout->setSingleStep(1000);
    spinTimeout->setSuffix(" ms");
    comboModel->setEditable(true);

    const QStringList local_models = LoadLocalModelIds();
    for (const auto& m : local_models) {
      comboModel->addItem(m);
    }

    AsrProviderData initial;
    if (existing != nullptr) {
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

    CommandEditorGroup cmd_editor;
    if (initial.type == kLocalType) {
      if (!initial.model.empty() &&
          comboModel->findText(QString::fromStdString(initial.model)) == -1) {
        comboModel->addItem(QString::fromStdString(initial.model));
      }
      comboModel->setCurrentText(QString::fromStdString(initial.model));
      cmd_editor.Clear();
    } else {
      comboModel->setCurrentText(QString());
      cmd_editor.Populate(initial.command, initial.args, initial.env);
    }
    spinTimeout->setValue(initial.timeout_ms > 0 ? initial.timeout_ms : spinTimeout->minimum());

    UpdateFieldState(comboType, comboModel, &cmd_editor, spinTimeout);
    QObject::connect(comboType, QOverload<int>::of(&QComboBox::currentIndexChanged), comboType,
                     [comboType, comboModel, &cmd_editor, spinTimeout]() {
                       UpdateFieldState(comboType, comboModel, &cmd_editor, spinTimeout);
                     });

    form->addRow(AsrProviderDialog::tr("Name:"), editName);
    form->addRow(AsrProviderDialog::tr("Type:"), comboType);
    form->addRow(AsrProviderDialog::tr("Model:"), comboModel);
    cmd_editor.AddToForm(form);
    form->addRow(AsrProviderDialog::tr("Timeout (ms):"), spinTimeout);

    if (!RunFormDialog(parent, title, form)) {
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
      QString env_error;
      if (!cmd_editor.Extract(&out_data->command, &out_data->args, &out_data->env, &env_error)) {
        QMessageBox::warning(parent, AsrProviderDialog::tr("Error"), env_error);
        continue;
      }
      if (out_data->command.empty()) {
        QMessageBox::warning(parent, AsrProviderDialog::tr("Error"),
                             AsrProviderDialog::tr("Command providers require a command."));
        continue;
      }
      out_data->model.clear();
    }
    return true;
  }
}

} // namespace vinput::gui
