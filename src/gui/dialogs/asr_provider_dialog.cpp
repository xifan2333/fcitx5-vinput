#include "dialogs/asr_provider_dialog.h"

#include <QtCore/qcontainerfwd.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qformlayout.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qspinbox.h>
#include <QtWidgets/qwidget.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"

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
  const CoreConfig config = ConfigManager::Get().Load();
  const ModelManager manager(CoreConfigView::ModelBaseDir().string());
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

AsrProviderData ResolveInitialProviderData(const AsrProviderData* existing) {
  if (existing != nullptr) {
    return *existing;
  }
  AsrProviderData initial;
#if VINPUT_ENABLE_LOCAL_ASR
  initial.type = kLocalType;
#else
  initial.type = kCommandType;
#endif
  initial.timeout_ms = 15000;
  return initial;
}

void PopulateInitialValues(const AsrProviderData& initial, bool has_existing, QLineEdit* editName,
                           QComboBox* comboType, QComboBox* comboModel,
                           CommandEditorGroup* cmd_editor, QSpinBox* spinTimeout) {
  editName->setText(QString::fromStdString(initial.id));
  editName->setReadOnly(has_existing);
  comboType->setCurrentIndex(comboType->findData(QString::fromStdString(initial.type)));

  if (initial.type == kLocalType) {
    if (!initial.model.empty() &&
        comboModel->findText(QString::fromStdString(initial.model)) == -1) {
      comboModel->addItem(QString::fromStdString(initial.model));
    }
    comboModel->setCurrentText(QString::fromStdString(initial.model));
    cmd_editor->Clear();
  } else {
    comboModel->setCurrentText(QString());
    cmd_editor->Populate(initial.command, initial.args, initial.env);
  }
  spinTimeout->setValue(initial.timeout_ms > 0 ? initial.timeout_ms : spinTimeout->minimum());
}

bool ValidateAndExtractProvider(QLineEdit* editName, QComboBox* comboType, QComboBox* comboModel,
                                CommandEditorGroup* cmd_editor, QSpinBox* spinTimeout,
                                AsrProviderData* out_data, QString* error) {
  const QString extracted_name = editName->text().trimmed();
  if (extracted_name.isEmpty()) {
    if (error != nullptr) {
      *error = AsrProviderDialog::tr("Provider name must not be empty.");
    }
    return false;
  }

  const QString extracted_type = comboType->currentData().toString();
  out_data->id = extracted_name.toStdString();
  out_data->type = extracted_type.toStdString();
  out_data->timeout_ms = spinTimeout->value();

#if VINPUT_ENABLE_LOCAL_ASR
  if (extracted_type == kLocalType) {
    out_data->model = comboModel->currentText().trimmed().toStdString();
    out_data->command.clear();
    out_data->args.clear();
    out_data->env.clear();
    return true;
  }
#endif

  std::string extracted_command;
  std::vector<std::string> extracted_args;
  std::map<std::string, std::string> extracted_env;
  QString env_error;
  if (!cmd_editor->Extract(&extracted_command, &extracted_args, &extracted_env, &env_error)) {
    if (error != nullptr) {
      *error = env_error;
    }
    return false;
  }
  if (extracted_command.empty()) {
    if (error != nullptr) {
      *error = AsrProviderDialog::tr("Command providers require a command.");
    }
    return false;
  }

  out_data->command = std::move(extracted_command);
  out_data->args = std::move(extracted_args);
  out_data->env = std::move(extracted_env);
  out_data->model.clear();
  return true;
}

} // namespace

bool ShowAsrProviderDialog(QWidget* parent, const QString& title, const AsrProviderData* existing,
                           AsrProviderData* out_data) {
  if (out_data == nullptr) {
    return false;
  }

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

  comboModel->addItems(LoadLocalModelIds());

  const AsrProviderData initial = ResolveInitialProviderData(existing);
  CommandEditorGroup cmd_editor;
  PopulateInitialValues(initial, existing != nullptr, editName, comboType, comboModel, &cmd_editor,
                        spinTimeout);

  UpdateFieldState(comboType, comboModel, &cmd_editor, spinTimeout);
  QObject::connect(comboType, &QComboBox::currentIndexChanged, comboType,
                   [comboType, comboModel, &cmd_editor, spinTimeout]() {
                     UpdateFieldState(comboType, comboModel, &cmd_editor, spinTimeout);
                   });

  form->addRow(AsrProviderDialog::tr("Name:"), editName);
  form->addRow(AsrProviderDialog::tr("Type:"), comboType);
  form->addRow(AsrProviderDialog::tr("Model:"), comboModel);
  cmd_editor.AddToForm(form);
  form->addRow(AsrProviderDialog::tr("Timeout (ms):"), spinTimeout);

  return RunValidatingFormDialog(parent, title, form, [&](QString* error) {
    return ValidateAndExtractProvider(editName, comboType, comboModel, &cmd_editor, spinTimeout,
                                      out_data, error);
  });
}

} // namespace vinput::gui
