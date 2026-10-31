#include "dialogs/adapter_dialog.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QtCore/qcoreapplication.h>

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
    cmd_editor.Populate(initial.command, initial.args, initial.env);
    cmd_editor.AddToForm(form);

    auto* chkAutoStart = new QCheckBox(AdapterDialog::tr("Start with daemon"));
    chkAutoStart->setChecked(initial.autoStart);
    form->addRow(QString(), chkAutoStart);

    if (!RunFormDialog(parent, AdapterDialog::tr("Configure LLM Adapter"), form)) {
      return false;
    }

    out_data->id = initial.id;
    out_data->autoStart = chkAutoStart->isChecked();
    QString env_error;
    if (!cmd_editor.Extract(&out_data->command, &out_data->args, &out_data->env, &env_error)) {
      QMessageBox::warning(parent, AdapterDialog::tr("Error"), env_error);
      continue;
    }

    return true;
  }
}

} // namespace vinput::gui
