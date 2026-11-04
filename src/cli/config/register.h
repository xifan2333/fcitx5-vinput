#pragma once

#include <CLI/CLI.hpp>

#include "cli/config/action.h"

#include "config.h"

namespace vinput::cli::config {

void RegisterConfigCli(CLI::App& app, CliAction* action);

void RegisterDeviceCommands(CLI::App& app, CliAction* action);
void RegisterInitCommands(CLI::App& app, CliAction* action);
#if VINPUT_ENABLE_LOCAL_ASR
void RegisterHotwordCommands(CLI::App& app, CliAction* action);
void RegisterModelCommands(CLI::App& app, CliAction* action);
#endif
void RegisterProviderCommands(CLI::App& app, CliAction* action);
void RegisterLlmCommands(CLI::App& app, CliAction* action);
void RegisterAdapterCommands(CLI::App& app, CliAction* action);
void RegisterSceneCommands(CLI::App& app, CliAction* action);
void RegisterConfigCommands(CLI::App& app, CliAction* action);

} // namespace vinput::cli::config
