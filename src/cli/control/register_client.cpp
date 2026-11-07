#include <CLI/CLI.hpp>
#include <memory>
#include <string>

#include "common/i18n.h"

#include "cli/control/action.h"
#include "cli/control/client_actions.h"
#include "cli/control/register.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli::control {

void RegisterClientCommands(CLI::App& app, CliAction* action) {
  // 1. vinput trigger (list, start, stop)
  auto* trigger = app.add_subcommand("trigger", _("Manage voice trigger clients"));
  trigger->require_subcommand(1);

  auto* trigger_list = trigger->add_subcommand("list", _("List configured triggers"));
  trigger_list->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) {
      return RunClientList(ClientKind::Trigger, fmt, ctx);
    };
  });

  auto trigger_start_id = std::make_shared<std::string>();
  auto* trigger_start = trigger->add_subcommand("start", _("Start a trigger"));
  trigger_start->add_option("id", *trigger_start_id, _("Trigger ID"))->required();
  trigger_start->callback([action, trigger_start_id]() {
    *action = [trigger_start_id](Formatter& fmt, const CliContext& ctx) {
      return RunClientStart(ClientKind::Trigger, *trigger_start_id, fmt, ctx);
    };
  });

  auto trigger_stop_id = std::make_shared<std::string>();
  auto* trigger_stop = trigger->add_subcommand("stop", _("Stop a trigger"));
  trigger_stop->add_option("id", *trigger_stop_id, _("Trigger ID"))->required();
  trigger_stop->callback([action, trigger_stop_id]() {
    *action = [trigger_stop_id](Formatter& fmt, const CliContext& ctx) {
      return RunClientStop(ClientKind::Trigger, *trigger_stop_id, fmt, ctx);
    };
  });

  // 2. vinput receiver (list, start, stop)
  auto* receiver = app.add_subcommand("receiver", _("Manage voice receiver clients"));
  receiver->require_subcommand(1);

  auto* receiver_list = receiver->add_subcommand("list", _("List configured receivers"));
  receiver_list->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) {
      return RunClientList(ClientKind::Receiver, fmt, ctx);
    };
  });

  auto receiver_start_id = std::make_shared<std::string>();
  auto* receiver_start = receiver->add_subcommand("start", _("Start a receiver"));
  receiver_start->add_option("id", *receiver_start_id, _("Receiver ID"))->required();
  receiver_start->callback([action, receiver_start_id]() {
    *action = [receiver_start_id](Formatter& fmt, const CliContext& ctx) {
      return RunClientStart(ClientKind::Receiver, *receiver_start_id, fmt, ctx);
    };
  });

  auto receiver_stop_id = std::make_shared<std::string>();
  auto* receiver_stop = receiver->add_subcommand("stop", _("Stop a receiver"));
  receiver_stop->add_option("id", *receiver_stop_id, _("Receiver ID"))->required();
  receiver_stop->callback([action, receiver_stop_id]() {
    *action = [receiver_stop_id](Formatter& fmt, const CliContext& ctx) {
      return RunClientStop(ClientKind::Receiver, *receiver_stop_id, fmt, ctx);
    };
  });

  // 3. vinput listen
  auto listen_type = std::make_shared<std::string>("committed");
  auto* listen = app.add_subcommand("listen", _("Listen to live speech events from daemon"));
  listen
      ->add_option("-t,--type", *listen_type,
                   _("Event type filter (committed, partial, status, all; default: committed)"))
      ->check(CLI::IsMember({"committed", "partial", "status", "all"}));
  listen->callback([action, listen_type]() {
    *action = [listen_type](Formatter& fmt, const CliContext& ctx) {
      return RunListen(*listen_type, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::control
