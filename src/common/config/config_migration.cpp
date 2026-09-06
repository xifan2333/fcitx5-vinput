#include "common/config/config_migration.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace vinput::migration {

namespace {

using json = nlohmann::json;

void RenameField(json& obj, std::string_view old_name, std::string_view new_name,
                 std::string_view context_desc, std::vector<MigrationChange>& changes) {
  if (!obj.is_object()) {
    return;
  }
  auto it = obj.find(old_name);
  if (it != obj.end()) {
    if (!obj.contains(new_name)) {
      obj[std::string(new_name)] = std::move(*it);
      changes.push_back({"config.json", std::string(context_desc) + ": renamed '" +
                                            std::string(old_name) + "' to '" +
                                            std::string(new_name) + "'"});
    }
    obj.erase(it);
  }
}

void RenameFieldInArray(json& arr, std::string_view old_name, std::string_view new_name,
                        std::string_view array_desc, std::vector<MigrationChange>& changes) {
  if (!arr.is_array()) {
    return;
  }
  for (size_t i = 0; i < arr.size(); ++i) {
    if (arr[i].is_object()) {
      std::string desc = std::string(array_desc) + "[" + std::to_string(i) + "]";
      if (arr[i].contains("id") && arr[i]["id"].is_string()) {
        desc += " (" + arr[i]["id"].get<std::string>() + ")";
      }
      RenameField(arr[i], old_name, new_name, desc, changes);
    }
  }
}

template <typename T>
void EnsureFieldInArray(json& arr, std::string_view field_name, const T& default_val,
                        std::string_view array_desc, std::vector<MigrationChange>& changes) {
  if (!arr.is_array()) {
    return;
  }
  for (size_t i = 0; i < arr.size(); ++i) {
    if (arr[i].is_object() && !arr[i].contains(field_name)) {
      arr[i][std::string(field_name)] = default_val;
      std::string desc = std::string(array_desc) + "[" + std::to_string(i) + "]";
      if (arr[i].contains("id") && arr[i]["id"].is_string()) {
        desc += " (" + arr[i]["id"].get<std::string>() + ")";
      }
      changes.push_back({"config.json", desc + ": added '" + std::string(field_name) + "'"});
    }
  }
}

bool StartsWithKey(std::string_view line, std::string_view key) {
  if (!line.starts_with(key)) {
    return false;
  }
  if (line.size() == key.size()) {
    return false;
  }
  const char next = line[key.size()];
  return next == '=' || next == ' ' || next == '\t';
}

void ReplaceIniKey(std::string& ini_text, std::string_view old_key, std::string_view new_key,
                   std::vector<MigrationChange>& changes) {
  std::istringstream iss(ini_text);
  std::ostringstream oss;
  std::string line;
  bool replaced = false;

  while (std::getline(iss, line)) {
    if (!replaced && StartsWithKey(line, old_key)) {
      const auto eq = line.find('=');
      if (eq != std::string::npos) {
        oss << new_key << line.substr(eq) << "\n";
        replaced = true;
        changes.push_back({"vinput.conf", "replaced key '" + std::string(old_key) + "' with '" +
                                              std::string(new_key) + "'"});
        continue;
      }
    }
    oss << line << "\n";
  }

  if (replaced) {
    ini_text = oss.str();
    if (!ini_text.empty() && ini_text.back() == '\n') {
      ini_text.pop_back();
    }
  }
}

void RemoveIniKey(std::string& ini_text, std::string_view key,
                  std::vector<MigrationChange>& changes) {
  std::istringstream iss(ini_text);
  std::ostringstream oss;
  std::string line;
  bool removed = false;

  while (std::getline(iss, line)) {
    if (StartsWithKey(line, key)) {
      removed = true;
      changes.push_back({"vinput.conf", "removed obsolete key '" + std::string(key) + "'"});
      continue;
    }
    oss << line << "\n";
  }

  if (removed) {
    ini_text = oss.str();
    if (!ini_text.empty() && ini_text.back() == '\n') {
      ini_text.pop_back();
    }
  }
}

std::string CurrentTimestampStr() {
  const auto now = std::chrono::system_clock::now();
  const auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::tm tm_now{};
  localtime_r(&time_t_now, &tm_now);
  std::ostringstream oss;
  oss << std::put_time(&tm_now, "%Y%m%d-%H%M%S");
  return oss.str();
}

struct MigrationStep {
  std::string_view version;
  std::string_view description;
  std::function<void(json& j, std::string& ini, std::vector<MigrationChange>& changes)> apply;
};

const std::vector<MigrationStep>& RegisteredSteps() {
  static const std::vector<MigrationStep> kSteps = {
      {
          "v2.3.15",
          "Normalize candidate_count to count and unify hotkeys into MenuKey",
          [](json& j, std::string& ini, std::vector<MigrationChange>& ch) {
            if (j.contains("scenes") && j["scenes"].is_object()) {
              RenameFieldInArray(j["scenes"]["definitions"], "candidate_count", "count",
                                 "scenes.definitions", ch);
              RenameFieldInArray(j["scenes"]["items"], "candidate_count", "count", "scenes.items",
                                 ch);
            }
            if (j.contains("llm") && j["llm"].is_object()) {
              RenameField(j["llm"], "adaptors", "adapters", "llm", ch);
            }
            ReplaceIniKey(ini, "SceneMenuKey", "MenuKey", ch);
            RemoveIniKey(ini, "AsrMenuKey", ch);
          },
      },
      {
          "v2.3.25",
          "Decouple show_raw into raw_cand and raw_prev",
          [](json& j, std::string& /*ini*/, std::vector<MigrationChange>& ch) {
            if (j.contains("scenes") && j["scenes"].is_object()) {
              RenameFieldInArray(j["scenes"]["definitions"], "show_raw", "raw_cand",
                                 "scenes.definitions", ch);
              RenameFieldInArray(j["scenes"]["items"], "show_raw", "raw_cand", "scenes.items", ch);
              EnsureFieldInArray(j["scenes"]["definitions"], "raw_prev", true, "scenes.definitions",
                                 ch);
              EnsureFieldInArray(j["scenes"]["items"], "raw_prev", true, "scenes.items", ch);
            }
          },
      },
  };
  return kSteps;
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

  json root_json;
  bool has_json = false;
  std::string json_content;
  std::string read_error;
  if (std::filesystem::exists(report.config_file)) {
    if (!vinput::file::ReadTextFile(report.config_file, &json_content, &read_error)) {
      report.error = "Failed to read " + report.config_file.string() + ": " + read_error;
      return report;
    }
    try {
      root_json = json::parse(json_content);
      has_json = true;
    } catch (const std::exception& ex) {
      report.error = "Failed to parse " + report.config_file.string() + ": " + ex.what();
      return report;
    }
  }

  std::string ini_content;
  bool has_ini = false;
  if (std::filesystem::exists(report.conf_file)) {
    if (vinput::file::ReadTextFile(report.conf_file, &ini_content, &read_error)) {
      has_ini = true;
    }
  }

  if (!has_json && !has_ini) {
    report.needed = false;
    return report;
  }

  for (const auto& step : RegisteredSteps()) {
    step.apply(root_json, ini_content, report.changes);
  }

  if (report.changes.empty()) {
    report.needed = false;
    return report;
  }

  report.needed = true;
  if (dry_run) {
    return report;
  }

  const std::string ts = CurrentTimestampStr();

  // 1. Back up and write config.json
  if (has_json) {
    const auto backup_dir = report.config_file.parent_path() / "backups";
    std::string err;
    vinput::file::EnsureParentDirectory(backup_dir / "placeholder", &err);
    const auto backup_file = backup_dir / ("config.json.bak." + ts);
    if (vinput::file::AtomicWriteTextFile(backup_file, json_content, &err)) {
      report.config_backup = backup_file;
    }

    const std::string updated_json = root_json.dump(4) + "\n";
    if (!vinput::file::AtomicWriteTextFile(report.config_file, updated_json, &err)) {
      report.error = "Failed to write updated " + report.config_file.string() + ": " + err;
      return report;
    }
  }

  // 2. Back up and write vinput.conf
  if (has_ini) {
    const auto backup_dir = report.conf_file.parent_path() / "backups";
    std::string err;
    vinput::file::EnsureParentDirectory(backup_dir / "placeholder", &err);
    const auto backup_file = backup_dir / ("vinput.conf.bak." + ts);
    if (vinput::file::AtomicWriteTextFile(backup_file, ini_content, &err)) {
      report.conf_backup = backup_file;
    }

    if (!vinput::file::AtomicWriteTextFile(report.conf_file, ini_content + "\n", &err)) {
      report.error = "Failed to write updated " + report.conf_file.string() + ": " + err;
      return report;
    }
  }

  report.applied = true;
  return report;
}

} // namespace vinput::migration
