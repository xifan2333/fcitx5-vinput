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

struct AsrProviderFields {
  QLineEdit* editName = nullptr;
  QComboBox* comboType = nullptr;
  QComboBox* comboModel = nullptr;
  CommandEditorGroup cmd_editor;
  QSpinBox* spinTimeout = nullptr;
};

void UpdateFieldState(const AsrProviderFields& fields) {
  const bool is_local = fields.comboType->currentData().toString() == kLocalType;
  fields.comboModel->setEnabled(is_local);
  fields.cmd_editor.SetEnabled(!is_local);
  fields.spinTimeout->setEnabled(true);
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

void BuildAsrProviderForm(AsrProviderFields& fields, QFormLayout* form) {
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

#if VINPUT_ENABLE_LOCAL_ASR
  fields.comboType->addItem(AsrProviderDialog::tr("local"), QString(kLocalType));
#endif
  fields.comboType->addItem(AsrProviderDialog::tr("command"), QString(kCommandType));
  fields.spinTimeout->setRange(1000, 300000);
  fields.spinTimeout->setSingleStep(1000);
  fields.spinTimeout->setSuffix(" ms");
  fields.comboModel->setEditable(true);

  fields.comboModel->addItems(LoadLocalModelIds());

  form->addRow(AsrProviderDialog::tr("Name:"), fields.editName);
  form->addRow(AsrProviderDialog::tr("Type:"), fields.comboType);
  form->addRow(AsrProviderDialog::tr("Model:"), fields.comboModel);
  fields.cmd_editor.AddToForm(form);
  form->addRow(AsrProviderDialog::tr("Timeout (ms):"), fields.spinTimeout);
}

void PopulateInitialValues(const AsrProviderData& initial, bool has_existing,
                           AsrProviderFields& fields) {
  fields.editName->setText(QString::fromStdString(initial.id));
  fields.editName->setReadOnly(has_existing);
  fields.comboType->setCurrentIndex(
      fields.comboType->findData(QString::fromStdString(initial.type)));

  if (initial.type == kLocalType) {
    if (!initial.model.empty() &&
        fields.comboModel->findText(QString::fromStdString(initial.model)) == -1) {
      fields.comboModel->addItem(QString::fromStdString(initial.model));
    }
    fields.comboModel->setCurrentText(QString::fromStdString(initial.model));
    fields.cmd_editor.Clear();
  } else {
    fields.comboModel->setCurrentText(QString());
    fields.cmd_editor.Populate(initial.command, initial.args, initial.env);
  }
  fields.spinTimeout->setValue(initial.timeout_ms > 0 ? initial.timeout_ms
                                                      : fields.spinTimeout->minimum());
}

bool ValidateAndExtractProvider(AsrProviderFields& fields, AsrProviderData* out_data,
                                QString* error) {
  const QString extracted_name = fields.editName->text().trimmed();
  if (extracted_name.isEmpty()) {
    if (error != nullptr) {
      *error = AsrProviderDialog::tr("Provider name must not be empty.");
    }
    return false;
  }

  const QString extracted_type = fields.comboType->currentData().toString();
  out_data->id = extracted_name.toStdString();
  out_data->type = extracted_type.toStdString();
  out_data->timeout_ms = fields.spinTimeout->value();

#if VINPUT_ENABLE_LOCAL_ASR
  if (extracted_type == kLocalType) {
    out_data->model = fields.comboModel->currentText().trimmed().toStdString();
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
  if (!fields.cmd_editor.Extract(&extracted_command, &extracted_args, &extracted_env, &env_error)) {
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
  AsrProviderFields fields{
      .editName = new QLineEdit(),
      .comboType = new QComboBox(),
      .comboModel = new QComboBox(),
      .cmd_editor = {},
      .spinTimeout = new QSpinBox(),
  };

  BuildAsrProviderForm(fields, form);

  const AsrProviderData initial = ResolveInitialProviderData(existing);
  PopulateInitialValues(initial, existing != nullptr, fields);

  UpdateFieldState(fields);
  QObject::connect(fields.comboType, &QComboBox::currentIndexChanged, fields.comboType,
                   [&fields]() { UpdateFieldState(fields); });

  return RunValidatingFormDialog(
      parent, title, form,
      [&](QString* error) { return ValidateAndExtractProvider(fields, out_data, error); },
      AsrProviderDialog::tr("Error"));
}

} // namespace vinput::gui
