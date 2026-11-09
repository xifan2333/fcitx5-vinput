#include "dialogs/client_dialog.h"

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

#include "common/config/core_config_types.h"

#include "utils/gui_helpers.h"

namespace vinput::gui {

namespace {

struct ClientDialog {
  Q_DECLARE_TR_FUNCTIONS(ClientDialog)
};

} // namespace

bool ShowClientDialog(QWidget* parent, const QString& title, const ClientSpec& initial,
                      ClientSpec* out_data) {
  if (out_data == nullptr) {
    return false;
  }

  ClientSpec current = initial;
  while (true) {
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    form->addRow(ClientDialog::tr("Client ID:"), new QLabel(QString::fromStdString(current.id)));

    auto* checkAutoStart = new QCheckBox(ClientDialog::tr("Auto-start with daemon"));
    checkAutoStart->setChecked(current.autoStart);
    form->addRow(QString(), checkAutoStart);

    CommandEditorGroup cmd_editor;
    cmd_editor.SetEnvMaxHeight(120);
    cmd_editor.Populate(current.command, current.args, current.env);
    cmd_editor.AddToForm(form);

    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    QString env_error;
    bool extract_ok = false;
    bool auto_start = false;

    const bool accepted = RunFormDialog(parent, title, form, [&]() {
      extract_ok = cmd_editor.Extract(&command, &args, &env, &env_error);
      auto_start = checkAutoStart->isChecked();
    });

    if (!accepted) {
      return false;
    }

    current.autoStart = auto_start;
    if (!extract_ok) {
      if (!command.empty()) {
        current.command = std::move(command);
      }
      current.args = std::move(args);
      QMessageBox::warning(parent, ClientDialog::tr("Error"), env_error);
      continue;
    }

    if (command.empty()) {
      current.args = std::move(args);
      current.env = std::move(env);
      QMessageBox::warning(parent, ClientDialog::tr("Error"),
                           ClientDialog::tr("Command must not be empty."));
      continue;
    }

    out_data->id = initial.id;
    out_data->command = std::move(command);
    out_data->args = std::move(args);
    out_data->env = std::move(env);
    out_data->autoStart = auto_start;
    return true;
  }
}

} // namespace vinput::gui
