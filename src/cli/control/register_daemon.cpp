#include <CLI/CLI.hpp>
#include <memory>

#include "common/i18n.h"

#include "cli/control/action.h"
#include "cli/control/daemon_actions.h"
#include "cli/control/register.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli::control {

void RegisterDaemonCommands(CLI::App& app, CliAction* action) {
  auto* daemon = app.add_subcommand("daemon", _("Control daemon lifecycle"));
  daemon->alias("d");
  daemon->require_subcommand(1);

  auto* status = daemon->add_subcommand("status", _("Show daemon status"));
  status->alias("st");
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
  auto level = std::make_shared<std::string>();
  auto* logs = daemon->add_subcommand("log", _("Show daemon logs"));
  logs->add_flag("-f,--follow", *follow, _("Follow log output"));
  logs->add_option("-n,--lines", *lines, _("Number of log lines"))->default_val(100);
  logs->add_option("-l,--level", *level, _("Log level filter (trace, debug, info, warn, error)"))
      ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error"}));
  logs->callback([action, follow, lines, level]() {
    *action = [follow, lines, level](Formatter& fmt, const CliContext& ctx) {
      return RunDaemonControlLogs(*follow, *lines, *level, fmt, ctx);
    };
  });

  auto pipeScene = std::make_shared<std::string>();
  auto pipeIme = std::make_shared<bool>(false);
  auto* pipe =
      daemon->add_subcommand("pipe", _("Stream audio from STDIN to daemon for transcription"));
  pipe->add_option("-s,--scene", *pipeScene, _("Scene id for postprocessing"));
  pipe->add_flag("--ime", *pipeIme, _("Commit recognized text to desktop IME input context"));
  pipe->callback([action, pipeScene, pipeIme]() {
    *action = [pipeScene, pipeIme](Formatter& fmt, const CliContext& ctx) {
      return RunDaemonControlPipe(*pipeScene, *pipeIme, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::control
