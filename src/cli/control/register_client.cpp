#include <CLI/CLI.hpp>
#include <memory>
#include <string>
#include <vector>

#include "common/i18n.h"

#include "cli/control/action.h"
#include "cli/control/client_actions.h"
#include "cli/control/register.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli::control {

namespace {

struct ClientLabels {
  const char* list_desc;
  const char* list_avail_desc;
  const char* add_desc;
  const char* id_desc;
  const char* short_id_desc;
  const char* update_desc;
  const char* update_all_desc;
  const char* edit_desc;
  const char* remove_desc;
  const char* start_desc;
  const char* stop_desc;
};

ClientLabels GetClientLabels(ClientKind kind) {
  if (kind == ClientKind::Trigger) {
    return {
        .list_desc = _("List configured triggers"),
        .list_avail_desc = _("List remote triggers"),
        .add_desc = _("Add a trigger"),
        .id_desc = _("Trigger ID"),
        .short_id_desc = _("Trigger short ID"),
        .update_desc = _("Update a trigger script"),
        .update_all_desc = _("Update all installed triggers"),
        .edit_desc = _("Edit a trigger script"),
        .remove_desc = _("Remove a trigger"),
        .start_desc = _("Start a trigger"),
        .stop_desc = _("Stop a trigger"),
    };
  }
  return {
      .list_desc = _("List configured receivers"),
      .list_avail_desc = _("List remote receivers"),
      .add_desc = _("Add a receiver"),
      .id_desc = _("Receiver ID"),
      .short_id_desc = _("Receiver short ID"),
      .update_desc = _("Update a receiver script"),
      .update_all_desc = _("Update all installed receivers"),
      .edit_desc = _("Edit a receiver script"),
      .remove_desc = _("Remove a receiver"),
      .start_desc = _("Start a receiver"),
      .stop_desc = _("Stop a receiver"),
  };
}

void RegisterClientSubcommands(CLI::App* sub, ClientKind kind, CliAction* action) {
  const auto labels = GetClientLabels(kind);

  auto available = std::make_shared<bool>(false);
  auto* list = sub->add_subcommand("list", labels.list_desc);
  list->alias("ls");
  list->add_flag("-a,--available", *available, labels.list_avail_desc);
  list->callback([action, kind, available]() {
    *action = [kind, available](Formatter& fmt, const CliContext& ctx) {
      return RunClientList(kind, *available, fmt, ctx);
    };
  });

  auto addId = std::make_shared<std::string>();
  auto envOverrides = std::make_shared<std::vector<std::string>>();
  auto* add = sub->add_subcommand("add", labels.add_desc);
  add->add_option("id", *addId, labels.id_desc)->required();
  add->add_option("-e,--env", *envOverrides, _("Environment variable as KEY=VALUE (repeatable)"))
      ->allow_extra_args(false);
  add->callback([action, kind, addId, envOverrides]() {
    *action = [kind, addId, envOverrides](Formatter& fmt, const CliContext& ctx) {
      return RunClientInstall(kind, *addId, *envOverrides, fmt, ctx);
    };
  });

  auto updateSelector = std::make_shared<std::string>();
  auto updateAll = std::make_shared<bool>(false);
  auto updateForce = std::make_shared<bool>(false);
  auto* update = sub->add_subcommand("update", labels.update_desc);
  update->alias("up");
  update->add_option("id", *updateSelector, labels.short_id_desc);
  update->add_flag("-a,--all", *updateAll, labels.update_all_desc);
  update->add_flag("-f,--force", *updateForce, _("Force update even if up to date"));
  update->callback([action, kind, updateSelector, updateAll, updateForce]() {
    *action = [kind, updateSelector, updateAll, updateForce](Formatter& fmt,
                                                             const CliContext& ctx) {
      return RunClientUpdate(kind, *updateSelector, *updateAll, *updateForce, fmt, ctx);
    };
  });

  auto editId = std::make_shared<std::string>();
  auto* edit = sub->add_subcommand("edit", labels.edit_desc);
  edit->alias("e");
  edit->add_option("id", *editId, labels.short_id_desc)->required();
  edit->callback([action, kind, editId]() {
    *action = [kind, editId](Formatter& fmt, const CliContext& ctx) {
      return RunClientEdit(kind, *editId, fmt, ctx);
    };
  });

  auto removeId = std::make_shared<std::string>();
  auto* remove = sub->add_subcommand("remove", labels.remove_desc);
  remove->alias("rm");
  remove->add_option("id", *removeId, labels.short_id_desc)->required();
  remove->callback([action, kind, removeId]() {
    *action = [kind, removeId](Formatter& fmt, const CliContext& ctx) {
      return RunClientRemove(kind, *removeId, fmt, ctx);
    };
  });

  auto startId = std::make_shared<std::string>();
  auto* start = sub->add_subcommand("start", labels.start_desc);
  start->add_option("id", *startId, labels.id_desc)->required();
  start->callback([action, kind, startId]() {
    *action = [kind, startId](Formatter& fmt, const CliContext& ctx) {
      return RunClientStart(kind, *startId, fmt, ctx);
    };
  });

  auto stopId = std::make_shared<std::string>();
  auto* stop = sub->add_subcommand("stop", labels.stop_desc);
  stop->add_option("id", *stopId, labels.id_desc)->required();
  stop->callback([action, kind, stopId]() {
    *action = [kind, stopId](Formatter& fmt, const CliContext& ctx) {
      return RunClientStop(kind, *stopId, fmt, ctx);
    };
  });
}

} // namespace

void RegisterClientCommands(CLI::App& app, CliAction* action) {
  auto* trigger = app.add_subcommand("trigger", _("Manage voice triggers"));
  trigger->alias("trig");
  trigger->require_subcommand(1);
  RegisterClientSubcommands(trigger, ClientKind::Trigger, action);

  auto* receiver = app.add_subcommand("receiver", _("Manage voice receivers"));
  receiver->alias("recv");
  receiver->require_subcommand(1);
  RegisterClientSubcommands(receiver, ClientKind::Receiver, action);
}

} // namespace vinput::cli::control
