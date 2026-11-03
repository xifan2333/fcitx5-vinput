#pragma once

#include <CLI/App.hpp>

#include "cli/control/action.h"

namespace vinput::cli::control {

void RegisterControlCli(CLI::App& app, CliAction* action);
void RegisterDaemonCommands(CLI::App& app, CliAction* action);
void RegisterRecordingCommands(CLI::App& app, CliAction* action);

} // namespace vinput::cli::control
