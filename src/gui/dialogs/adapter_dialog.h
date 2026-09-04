#pragma once

#include <QCheckBox>
#include <QWidget>
#include <map>
#include <string>
#include <vector>

namespace vinput::gui {

// LLM adapter config data for the dialog.
struct AdapterData {
  std::string id;
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  bool autoStart{false};
};

// Show a dialog to edit an adapter's command/args/env.
// Returns true if accepted, fills out_data.
bool ShowAdapterDialog(QWidget* parent, const AdapterData& initial, AdapterData* out_data);

} // namespace vinput::gui
