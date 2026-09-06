#include <CLI/CLI.hpp>
#include <memory>

#include "common/i18n.h"

#include "cli/config/action.h"
#include "cli/config/config_actions.h"

namespace vinput::cli::config {

void RegisterConfigCommands(CLI::App& app, CliAction* action) {
  auto* config = app.add_subcommand("config", _("Read or write configuration values"));
  config->require_subcommand(1);

  auto getPath = std::make_shared<std::string>();
  auto* get = config->add_subcommand("get", _("Get a config value by JSON Pointer"));
  get->add_option("path", *getPath, _("JSON Pointer (e.g. /global/capture_device)"))->required();
  get->callback([action, getPath]() {
    *action = [getPath](Formatter& fmt, const CliContext& ctx) {
      return RunConfigDomainGet(*getPath, fmt, ctx);
    };
  });

  auto setPath = std::make_shared<std::string>();
  auto setValue = std::make_shared<std::string>();
  auto setStdin = std::make_shared<bool>(false);
  auto* set = config->add_subcommand("set", _("Set a config value by JSON Pointer"));
  set->add_option("path", *setPath, _("JSON Pointer (e.g. /global/capture_device)"))->required();
  set->add_option("value", *setValue, _("New value"));
  set->add_flag("-i,--stdin", *setStdin, _("Read value from stdin"));
  set->callback([action, setPath, setValue, setStdin]() {
    *action = [setPath, setValue, setStdin](Formatter& fmt, const CliContext& ctx) {
      return RunConfigDomainSet(*setPath, *setValue, *setStdin, fmt, ctx);
    };
  });

  auto editTarget = std::make_shared<std::string>();
  auto* edit = config->add_subcommand("edit", _("Open config file in editor"));
  edit->alias("e");
  edit->add_option("target", *editTarget, _("Config target: fcitx or core"))->required();
  edit->callback([action, editTarget]() {
    *action = [editTarget](Formatter& fmt, const CliContext& ctx) {
      return RunConfigDomainEdit(*editTarget, fmt, ctx);
    };
  });

  auto dryRun = std::make_shared<bool>(false);
  auto* migrate =
      config->add_subcommand("migrate", _("Migrate legacy configuration files to current format"));
  migrate->add_flag("-n,--dry-run", *dryRun,
                    _("Preview migration changes without modifying files"));
  migrate->callback([action, dryRun]() {
    *action = [dryRun](Formatter& fmt, const CliContext& ctx) {
      return RunConfigMigrate(*dryRun, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::config
