#include <CLI/App.hpp>

#include "common/i18n.h"

#include "cli/control/action.h"
#include "cli/control/daemon_actions.h"
#include "cli/control/register.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli::control {

void RegisterDaemonCommands(CLI::App& app, CliAction* action) {
  auto* daemon = app.add_subcommand("daemon", _("Control daemon lifecycle"));
  daemon->require_subcommand(1);

  auto* status = daemon->add_subcommand("status", _("Show daemon status"));
  status->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) {
      return RunDaemonControlStatus(fmt, ctx);
    };
  });

  auto* start = daemon->add_subcommand("start", _("Start daemon"));
  start->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) { return RunDaemonControlStart(fmt, ctx); };
  });

  auto* stop = daemon->add_subcommand("stop", _("Stop daemon"));
  stop->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) { return RunDaemonControlStop(fmt, ctx); };
  });

  auto* restart = daemon->add_subcommand("restart", _("Restart daemon"));
  restart->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) {
      return RunDaemonControlRestart(fmt, ctx);
    };
  });

  auto follow = std::make_shared<bool>(false);
  auto lines = std::make_shared<int>(100);
  auto* logs = daemon->add_subcommand("log", _("Show daemon logs"));
  logs->add_flag("-f,--follow", *follow, _("Follow log output"));
  logs->add_option("-n,--lines", *lines, _("Number of log lines"))->default_val(100);
  logs->callback([action, follow, lines]() {
    *action = [follow, lines](Formatter& fmt, const CliContext& ctx) {
      return RunDaemonControlLogs(*follow, *lines, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::control
