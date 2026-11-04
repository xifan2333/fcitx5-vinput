#include "cli/config/register.h"

#include <CLI/CLI.hpp>

#include "cli/config/action.h"

#include "config.h"

namespace vinput::cli::config {

void RegisterConfigCli(CLI::App& app, CliAction* action) {
  RegisterInitCommands(app, action);
#if VINPUT_ENABLE_LOCAL_ASR
  RegisterModelCommands(app, action);
  RegisterHotwordCommands(app, action);
#endif
  RegisterProviderCommands(app, action);
  RegisterLlmCommands(app, action);
  RegisterAdapterCommands(app, action);
  RegisterDeviceCommands(app, action);
  RegisterSceneCommands(app, action);
  RegisterConfigCommands(app, action);
}

} // namespace vinput::cli::config
