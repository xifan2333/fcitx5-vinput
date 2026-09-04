#include "dialogs/adapter_dialog.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QTextEdit>
#include <QVBoxLayout>

#include "utils/gui_helpers.h"

namespace vinput::gui {

namespace {

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

} // namespace

bool ShowAdapterDialog(QWidget* parent, const AdapterData& initial, AdapterData* out_data) {
  if (!out_data) {
    return false;
  }

  while (true) {
    QDialog dialog(parent);
    dialog.setWindowTitle(GuiTranslate("Configure LLM Adapter"));

    auto* layout = new QVBoxLayout(&dialog);

    auto* form = new QFormLayout();
    auto* editCommand = new QLineEdit();
    auto* textArgs = new QTextEdit();
    auto* textEnv = new QTextEdit();

    textArgs->setMaximumHeight(90);
    textEnv->setMaximumHeight(120);
    textArgs->setPlaceholderText(GuiTranslate("One argument per line"));
    textEnv->setPlaceholderText(GuiTranslate("One KEY=VALUE entry per line"));
    editCommand->setPlaceholderText(GuiTranslate("Command or interpreter"));

    editCommand->setText(QString::fromStdString(initial.command));
    textArgs->setPlainText(JoinArgLines(initial.args));
    textEnv->setPlainText(JoinEnvLines(initial.env));

    form->addRow(GuiTranslate("Adapter ID:"), new QLabel(QString::fromStdString(initial.id)));
    form->addRow(GuiTranslate("Command / Interpreter:"), editCommand);
    form->addRow(GuiTranslate("Args:"), textArgs);
    form->addRow(GuiTranslate("Env:"), textEnv);

    auto* chkAutoStart = new QCheckBox(GuiTranslate("Start with daemon"));
    chkAutoStart->setChecked(initial.autoStart);
    form->addRow(QString(), chkAutoStart);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
      return false;
    }

    out_data->id = initial.id;
    out_data->command = editCommand->text().trimmed().toStdString();
    out_data->autoStart = chkAutoStart->isChecked();
    out_data->args.clear();
    for (const QString& arg : NonEmptyLines(textArgs->toPlainText())) {
      out_data->args.push_back(arg.toStdString());
    }
    QString env_error;
    if (!ParseCommandEnv(textEnv->toPlainText(), &out_data->env, &env_error)) {
      QMessageBox::warning(parent, GuiTranslate("Error"), env_error);
      continue;
    }

    return true;
  }
}

} // namespace vinput::gui
