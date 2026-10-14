#include "common/config/config_migration.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "common/utils/path_utils.h"
#include "common/utils/process_utils.h"

#include "config.h"

namespace vinput::migration {

namespace {

std::filesystem::path ResolveMigrationScriptPath() {
  const char* env_path = std::getenv("VINPUT_MIGRATE_SCRIPT");
  if (env_path != nullptr && env_path[0] != '\0') {
    std::filesystem::path p(env_path);
    if (std::filesystem::exists(p)) {
      return p;
    }
  }

  const std::filesystem::path dev_path = VINPUT_MIGRATE_SCRIPT_SOURCE_PATH;
  if (std::filesystem::exists(dev_path)) {
    return dev_path;
  }

  const std::filesystem::path install_path = VINPUT_MIGRATE_SCRIPT_INSTALL_PATH;
  if (std::filesystem::exists(install_path)) {
    return install_path;
  }

  return {};
}

} // namespace

bool HasPendingMigrations(const std::filesystem::path& config_path,
                          const std::filesystem::path& conf_path) {
  const auto report = RunConfigMigration(true, config_path, conf_path);
  return report.needed;
}

MigrationReport RunConfigMigration(bool dry_run, const std::filesystem::path& config_path,
                                   const std::filesystem::path& conf_path) {
  MigrationReport report;
  report.config_file = config_path.empty() ? vinput::path::CoreConfigPath() : config_path;
  report.conf_file = conf_path.empty() ? vinput::path::FcitxAddonConfigPath() : conf_path;

  const auto script_path = ResolveMigrationScriptPath();
  if (script_path.empty()) {
    report.error = "Migration script not found";
    return report;
  }

  vinput::process::CommandSpec spec;
  spec.command = script_path.string();
  spec.args = {"--json"};
  if (dry_run) {
    spec.args.push_back("--dry-run");
  }
  spec.args.push_back("--config");
  spec.args.push_back(report.config_file.string());
  spec.args.push_back("--conf");
  spec.args.push_back(report.conf_file.string());
  spec.timeout_ms = 15000;

  const auto res = vinput::process::RunCommandWithInput(spec, {});
  if (res.launch_failed) {
    report.error = "Failed to launch migration script: " + res.stderr_text;
    return report;
  }
  if (res.timed_out) {
    report.error = "Migration script timed out";
    return report;
  }

  try {
    const auto j = nlohmann::json::parse(res.stdout_text);
    report.needed = j.value("needed", false);
    report.applied = j.value("applied", false);
    report.config_backup = j.value("config_backup", "");
    report.conf_backup = j.value("conf_backup", "");
    report.error = j.value("error", "");
    if (j.contains("changes") && j["changes"].is_array()) {
      for (const auto& ch : j["changes"]) {
        if (ch.is_object()) {
          report.changes.push_back(MigrationChange{
              .file = ch.value("file", ""),
              .description = ch.value("description", ""),
          });
        }
      }
    }
  } catch (const std::exception& ex) {
    report.error = "Failed to parse migration script JSON output: " + std::string(ex.what()) +
                   "\nOutput: " + res.stdout_text + "\nStderr: " + res.stderr_text;
    return report;
  }

  return report;
}

} // namespace vinput::migration
