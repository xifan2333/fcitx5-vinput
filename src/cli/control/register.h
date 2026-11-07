#pragma once

#include <CLI/CLI.hpp>

#include "cli/control/action.h"

namespace vinput::cli::control {

void RegisterControlCli(CLI::App& app, CliAction* action);
void RegisterDaemonCommands(CLI::App& app, CliAction* action);
void RegisterRecordingCommands(CLI::App& app, CliAction* action);
void RegisterClientCommands(CLI::App& app, CliAction* action);

} // namespace vinput::cli::control
