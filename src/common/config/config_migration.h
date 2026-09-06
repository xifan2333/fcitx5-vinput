#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vinput::migration {

struct MigrationChange {
  std::string file;
  std::string description;
};

struct MigrationReport {
  bool needed = false;
  bool applied = false;
  std::filesystem::path config_file;
  std::filesystem::path conf_file;
  std::filesystem::path config_backup;
  std::filesystem::path conf_backup;
  std::vector<MigrationChange> changes;
  std::string error;
};

// Check if any registered migration steps have pending modifications.
bool HasPendingMigrations(const std::filesystem::path& config_path = {},
                          const std::filesystem::path& conf_path = {});

// Run all registered migrations in order, with automatic backups before modifying files.
MigrationReport RunConfigMigration(bool dry_run = false,
                                   const std::filesystem::path& config_path = {},
                                   const std::filesystem::path& conf_path = {});

} // namespace vinput::migration
