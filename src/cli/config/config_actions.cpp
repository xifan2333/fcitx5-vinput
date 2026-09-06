#include "cli/config/config_actions.h"

#include <cstdio>
#include <iostream>
#include <string>

#include "common/config/config_migration.h"
#include "common/config/config_router.h"
#include "common/i18n.h"
#include "common/utils/string_utils.h"

#include "cli/utils/editor_utils.h"

int RunConfigDomainGet(const std::string& path, Formatter& fmt, const CliContext& ctx) {
  std::string value, error;
  if (!vinput::config::GetConfigValue(path, &value, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  if (ctx.json_output) {
    fmt.PrintJson({{"path", path}, {"value", value}});
  } else {
    std::puts(value.c_str());
  }
  return 0;
}

int RunConfigDomainSet(const std::string& path, const std::string& value_arg, bool from_stdin,
                       Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  std::string value = value_arg;
  if (from_stdin) {
    std::string line, all;
    while (std::getline(std::cin, line)) {
      if (!all.empty())
        all += '\n';
      all += line;
    }
    value = all;
  }
  std::string error;
  if (!vinput::config::SetConfigValue(path, value, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  fmt.PrintSuccess(_("Config value set."));
  return 0;
}

int RunConfigDomainEdit(const std::string& target, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  if (target != "core" && target != "fcitx") {
    fmt.PrintError(_("Unsupported config target. Use 'core' or 'fcitx'."));
    return 1;
  }
  auto file_path = vinput::config::GetEditTarget(target);
  return OpenInEditor(file_path);
}

int RunConfigMigrate(bool dry_run, Formatter& fmt, const CliContext& ctx) {
  const auto report = vinput::migration::RunConfigMigration(dry_run);

  if (!report.error.empty()) {
    fmt.PrintError(report.error);
    return 1;
  }

  if (ctx.json_output) {
    nlohmann::json j;
    j["needed"] = report.needed;
    j["applied"] = report.applied;
    j["dry_run"] = dry_run;
    j["config_file"] = report.config_file.string();
    j["conf_file"] = report.conf_file.string();
    j["config_backup"] = report.config_backup.string();
    j["conf_backup"] = report.conf_backup.string();
    nlohmann::json changes = nlohmann::json::array();
    for (const auto& ch : report.changes) {
      changes.push_back({{"file", ch.file}, {"description", ch.description}});
    }
    j["changes"] = changes;
    fmt.PrintJson(j);
    return 0;
  }

  if (!report.needed) {
    fmt.PrintSuccess(_("Configuration is already up to date. No migration needed."));
    return 0;
  }

  if (dry_run) {
    fmt.PrintSuccess(_("Pending migration changes:"));
  } else {
    fmt.PrintSuccess(_("Configuration migration completed successfully:"));
  }

  if (!report.config_backup.empty()) {
    fmt.PrintInfo(
        vinput::str::FmtStr(_("Backed up core config to: %s"), report.config_backup.c_str()));
  }
  if (!report.conf_backup.empty()) {
    fmt.PrintInfo(
        vinput::str::FmtStr(_("Backed up addon config to: %s"), report.conf_backup.c_str()));
  }

  const std::vector<std::string> headers = {_("FILE"), _("CHANGE")};
  std::vector<std::vector<std::string>> rows;
  for (const auto& ch : report.changes) {
    rows.push_back({ch.file, ch.description});
  }
  fmt.PrintTable(headers, rows);

  return 0;
}
