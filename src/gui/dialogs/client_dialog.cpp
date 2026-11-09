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

  while (true) {
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    form->addRow(ClientDialog::tr("Client ID:"), new QLabel(QString::fromStdString(initial.id)));

    auto* checkAutoStart = new QCheckBox(ClientDialog::tr("Auto-start with daemon"));
    checkAutoStart->setChecked(initial.autoStart);
    form->addRow(QString(), checkAutoStart);

    CommandEditorGroup cmd_editor;
    cmd_editor.SetEnvMaxHeight(120);
    cmd_editor.Populate(initial.command, initial.args, initial.env);
    cmd_editor.AddToForm(form);

    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    QString env_error;
    bool extract_ok = false;

    const bool accepted = RunFormDialog(parent, title, form, [&]() {
      extract_ok = cmd_editor.Extract(&command, &args, &env, &env_error);
    });

    if (!accepted) {
      return false;
    }

    if (!extract_ok) {
      QMessageBox::warning(parent, ClientDialog::tr("Error"), env_error);
      continue;
    }

    out_data->id = initial.id;
    out_data->command = std::move(command);
    out_data->args = std::move(args);
    out_data->env = std::move(env);
    out_data->autoStart = checkAutoStart->isChecked();
    return true;
  }
}

} // namespace vinput::gui
