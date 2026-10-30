#include <CLI/CLI.hpp>
#include <memory>

#include "common/i18n.h"

#include "cli/config/action.h"
#include "cli/config/device_actions.h"

namespace vinput::cli::config {

void RegisterDeviceCommands(CLI::App& app, CliAction* action) {
  auto* device = app.add_subcommand("device", _("Manage capture devices"));
  device->require_subcommand(1);

  auto* list = device->add_subcommand("list", _("List available audio input devices"));
  list->alias("ls");
  list->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) { return RunDeviceConfigList(fmt, ctx); };
  });

  auto id = std::make_shared<std::string>();
  auto* use = device->add_subcommand("use", _("Set active capture device"));
  use->add_option("id", *id, _("Device id, 'default', 'auto', or a comma-separated priority list"))
      ->required();
  use->callback([action, id]() {
    *action = [id](Formatter& fmt, const CliContext& ctx) {
      return RunDeviceConfigUse(*id, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::config
