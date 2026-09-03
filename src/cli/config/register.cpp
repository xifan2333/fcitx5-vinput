#include "cli/config/register.h"

#include <CLI/CLI.hpp>

#include "cli/config/action.h"

#include "config.h"

namespace vinput::cli::config {

void RegisterDeviceCommands(CLI::App& app, CliAction* action);
void RegisterHotwordCommands(CLI::App& app, CliAction* action);
void RegisterInitCommands(CLI::App& app, CliAction* action);
#if VINPUT_ENABLE_LOCAL_ASR
void RegisterModelCommands(CLI::App& app, CliAction* action);
#endif
void RegisterProviderCommands(CLI::App& app, CliAction* action);
void RegisterLlmCommands(CLI::App& app, CliAction* action);
void RegisterAdapterCommands(CLI::App& app, CliAction* action);
void RegisterSceneCommands(CLI::App& app, CliAction* action);
void RegisterConfigCommands(CLI::App& app, CliAction* action);

void RegisterConfigCli(CLI::App& app, CliAction* action) {
  RegisterInitCommands(app, action);
#if VINPUT_ENABLE_LOCAL_ASR
  RegisterModelCommands(app, action);
#endif
  RegisterProviderCommands(app, action);
  RegisterLlmCommands(app, action);
  RegisterAdapterCommands(app, action);
  RegisterHotwordCommands(app, action);
  RegisterDeviceCommands(app, action);
  RegisterSceneCommands(app, action);
  RegisterConfigCommands(app, action);
}

} // namespace vinput::cli::config
