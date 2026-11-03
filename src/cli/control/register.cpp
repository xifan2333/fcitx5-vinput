#include "cli/control/register.h"

#include <CLI/App.hpp>

#include "cli/control/action.h"

namespace vinput::cli::control {

void RegisterControlCli(CLI::App& app, CliAction* action) {
  RegisterDaemonCommands(app, action);
  RegisterRecordingCommands(app, action);
}

} // namespace vinput::cli::control
