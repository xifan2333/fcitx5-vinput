#include "dialogs/adapter_dialog.h"

#include <QtCore/qcoreapplication.h>
#include <QtCore/qstring.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qformlayout.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qwidget.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "utils/gui_helpers.h"

namespace vinput::gui {

namespace {

struct AdapterDialog {
  Q_DECLARE_TR_FUNCTIONS(AdapterDialog)
};

} // namespace

bool ShowAdapterDialog(QWidget* parent, const AdapterData& initial, AdapterData* out_data) {
  if (out_data == nullptr) {
    return false;
  }

  while (true) {
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    form->addRow(AdapterDialog::tr("Adapter ID:"), new QLabel(QString::fromStdString(initial.id)));

    CommandEditorGroup cmd_editor;
    cmd_editor.SetEnvMaxHeight(120);
    cmd_editor.Populate(initial.command, initial.args, initial.env);
    cmd_editor.AddToForm(form);

    auto* chkAutoStart = new QCheckBox(AdapterDialog::tr("Start with daemon"));
    chkAutoStart->setChecked(initial.autoStart);
    form->addRow(QString(), chkAutoStart);

    bool auto_start = false;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    QString env_error;
    bool extract_ok = false;

    const bool accepted =
        RunFormDialog(parent, AdapterDialog::tr("Configure LLM Adapter"), form, [&]() {
          auto_start = chkAutoStart->isChecked();
          extract_ok = cmd_editor.Extract(&command, &args, &env, &env_error);
        });

    if (!accepted) {
      return false;
    }

    if (!extract_ok) {
      QMessageBox::warning(parent, AdapterDialog::tr("Error"), env_error);
      continue;
    }

    out_data->id = initial.id;
    out_data->autoStart = auto_start;
    out_data->command = std::move(command);
    out_data->args = std::move(args);
    out_data->env = std::move(env);
    return true;
  }
}

} // namespace vinput::gui
