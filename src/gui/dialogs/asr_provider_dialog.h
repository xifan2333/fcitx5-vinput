#pragma once

#include <QString>
#include <QWidget>
#include <map>
#include <string>
#include <vector>

namespace vinput::gui {

// ASR provider data for the dialog.
struct AsrProviderData {
  std::string id;
  std::string type; // "local" or "command"
  std::string model;
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  int timeout_ms = 15000;
};

// Show a dialog to add or edit an ASR provider.
// Returns true if accepted, fills out_data.
// If existing is non-null, the dialog is in edit mode.
bool ShowAsrProviderDialog(QWidget* parent, const QString& title, const AsrProviderData* existing,
                           AsrProviderData* out_data);

} // namespace vinput::gui
