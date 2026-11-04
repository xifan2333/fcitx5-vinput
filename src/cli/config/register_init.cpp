#include <CLI/CLI.hpp>
#include <memory>

#include "common/i18n.h"

#include "cli/config/action.h"
#include "cli/config/init_actions.h"
#include "cli/config/register.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli::config {

void RegisterInitCommands(CLI::App& app, CliAction* action) {
  auto force = std::make_shared<bool>(false);
  auto* init = app.add_subcommand("init", _("Initialize default config and directories"));
  init->add_flag("-f,--force", *force, _("Overwrite existing config"));
  init->callback([action, force]() {
    *action = [force](Formatter& fmt, const CliContext& ctx) {
      return RunConfigInit(*force, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::config
