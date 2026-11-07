#include "cli/control/register.h"

#include <CLI/CLI.hpp>

#include "cli/control/action.h"

namespace vinput::cli::control {

void RegisterControlCli(CLI::App& app, CliAction* action) {
  RegisterDaemonCommands(app, action);
  RegisterRecordingCommands(app, action);
  RegisterClientCommands(app, action);
}

} // namespace vinput::cli::control
