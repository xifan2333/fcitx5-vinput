#include "common/config/config_migration.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <filesystem>
#include <glaze/core/opts.hpp>
#include <glaze/core/read.hpp>
#include <glaze/core/write.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/write.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/version_utils.h"

namespace vinput::migration {

namespace {

// Migration rewrites user-edited trees, which may carry keys unknown to this
// version, so unknown keys must not abort the parse.
constexpr auto kMigrationReadOpts = glz::opts{.error_on_unknown_keys = uint8_t{0}};
// Migrated `config.json` files keep the historical two space indentation.
constexpr auto kMigrationWriteOpts = glz::opts{.prettify = uint8_t{1}, .indentation_width = 2};

std::string FormatJson(const glz::json_t& value) {
  std::string out;
  if (bool(glz::write_json(value, out))) {
    return {};
  }
  return out;
}

std::string FormatPrettyJson(const glz::json_t& value) {
  std::string out;
  if (bool(glz::write<kMigrationWriteOpts>(value, out))) {
    return {};
  }
  return out;
}

enum class ActionType : std::uint8_t {
  Rename,
  Remove,
  Ensure,
  ReplaceIfDefault,
  IniReplaceKey,
  IniRemoveKey,
};

struct MigrationRule {
  std::string_view version;
  std::string_view target_file;
  ActionType action;
  std::string_view path;
  std::string_view new_key;
  glz::json_t default_value;
  glz::json_t expected_value;
  glz::json_t to_value;

  MigrationRule(std::string_view v, std::string_view tf, ActionType act, std::string_view p,
                std::string_view nk = {}, glz::json_t def = {}, glz::json_t exp = {},
                glz::json_t to = {})
      : version(v), target_file(tf), action(act), path(p), new_key(nk),
        default_value(std::move(def)), expected_value(std::move(exp)), to_value(std::move(to)) {}
};

const std::vector<MigrationRule>& GetRules() {
  static const std::vector<MigrationRule> kRules = {
      // v2.3.15: Normalize candidate_count to count and adaptors to adapters
      {"2.3.15", "config.json", ActionType::Rename, "scenes.*.*.candidate_count", "count"},
      {"2.3.15", "config.json", ActionType::Rename, "llm.adaptors", "adapters"},
      {"2.3.15", "vinput.conf", ActionType::IniReplaceKey, "SceneMenuKey", "MenuKey"},
      {"2.3.15", "vinput.conf", ActionType::IniRemoveKey, "AsrMenuKey"},

      // v2.3.25: Decouple show_raw into raw_cand and raw_prev
      {"2.3.25", "config.json", ActionType::Rename, "scenes.*.*.show_raw", "raw_cand"},
      {"2.3.25", "config.json", ActionType::Ensure, "scenes.*.*.raw_prev", {}, true},

      // v2.3.27: Remove obsolete global.default_language
      {"2.3.27", "config.json", ActionType::Remove, "global.default_language"},

      // v2.4.0: Remove obsolete HoldActivationDelay from addon conf
      {"2.4.0", "vinput.conf", ActionType::IniRemoveKey, "HoldActivationDelay"},

      // v2.4.0: Upgrade legacy VAD acoustic defaults
      {"2.4.0",
       "config.json",
       ActionType::ReplaceIfDefault,
       "asr.vad.threshold",
       {},
       {},
       0.45,
       0.35},
      {"2.4.0",
       "config.json",
       ActionType::ReplaceIfDefault,
       "asr.vad.min_speech_duration",
       {},
       {},
       0.15,
       0.10},
      {"2.4.0",
       "config.json",
       ActionType::ReplaceIfDefault,
       "asr.vad.min_silence_duration",
       {},
       {},
       0.5,
       1.2},
      {"2.4.0",
       "config.json",
       ActionType::ReplaceIfDefault,
       "asr.vad.speech_pad_ms",
       {},
       {},
       30,
       500},
      {"2.4.0",
       "config.json",
       ActionType::ReplaceIfDefault,
       "asr.vad.speech_pad_ms",
       {},
       {},
       300,
       500},
  };
  return kRules;
}

vinput::version::SemanticVersion GetMaxRuleVersion() {
  vinput::version::SemanticVersion max_ver{};
  for (const auto& rule : GetRules()) {
    const auto parsed = vinput::version::Parse(rule.version);
    if (parsed.has_value() && max_ver < *parsed) {
      max_ver = *parsed;
    }
  }
  return max_ver;
}

std::string GetTargetVersionConstraint() {
  const auto max_ver = GetMaxRuleVersion();
  return ">=" + std::to_string(max_ver.major) + "." + std::to_string(max_ver.minor) + "." +
         std::to_string(max_ver.patch);
}

std::optional<vinput::version::SemanticVersion>
ParseConfigLowerBoundVersion(std::string_view ver_str) {
  ver_str = vinput::str::Trim(ver_str);
  if (ver_str.empty()) {
    return std::nullopt;
  }
  const auto comma_pos = ver_str.find(',');
  if (comma_pos != std::string_view::npos) {
    ver_str = ver_str.substr(0, comma_pos);
  }
  while (!ver_str.empty() &&
         (ver_str.front() == '>' || ver_str.front() == '=' || ver_str.front() == ' ')) {
    ver_str.remove_prefix(1);
  }
  return vinput::version::Parse(ver_str);
}

std::string GenerateTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  localtime_r(&time_t_now, &tm_buf);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);
  return {buf};
}

// Deep equality for the scalar values migration rules ever compare. Container
// values are never compared, so they report "not equal".
bool JsonEquals(const glz::json_t& actual, const glz::json_t& expected) {
  if (expected.is_number() && actual.is_number()) {
    return std::abs(*actual.get_if<double>() - *expected.get_if<double>()) < 1e-4;
  }
  if (const auto* lhs = actual.get_if<bool>()) {
    const auto* rhs = expected.get_if<bool>();
    return rhs != nullptr && *lhs == *rhs;
  }
  if (const auto* lhs = actual.get_if<std::string>()) {
    const auto* rhs = expected.get_if<std::string>();
    return rhs != nullptr && *lhs == *rhs;
  }
  return actual.is_null() && expected.is_null();
}

void EraseMember(glz::json_t& node, const std::string& key) {
  auto* object = node.get_if<glz::json_t::object_t>();
  if (object != nullptr) {
    object->erase(key);
  }
}

bool ApplyActionToTarget(glz::json_t& parent, const std::string& key, std::string_view desc,
                         const MigrationRule& rule, std::vector<MigrationChange>& changes) {
  const std::string desc_prefix = desc.empty() ? key : std::string(desc) + "." + key;

  switch (rule.action) {
  case ActionType::Rename: {
    if (parent.is_object() && parent.contains(key)) {
      const std::string new_key(rule.new_key);
      auto& object = parent.get_object();
      if (!parent.contains(new_key)) {
        object.emplace(new_key, std::move(object[key]));
        object.erase(key);
        changes.push_back({
            .file = "config.json",
            .description = desc_prefix + ": renamed '" + key + "' to '" + new_key + "'",
        });
      } else {
        EraseMember(parent, key);
        changes.push_back({
            .file = "config.json",
            .description = desc_prefix + ": removed obsolete duplicate key '" + key + "'",
        });
      }
      return true;
    }
    return false;
  }
  case ActionType::Remove: {
    if (parent.is_object() && parent.contains(key)) {
      EraseMember(parent, key);
      changes.push_back({
          .file = "config.json",
          .description =
              (desc.empty() ? key : std::string(desc)) + ": removed obsolete field '" + key + "'",
      });
      return true;
    }
    return false;
  }
  case ActionType::Ensure: {
    if (parent.is_object() && !parent.contains(key)) {
      parent[key] = rule.default_value;
      changes.push_back({
          .file = "config.json",
          .description = (desc.empty() ? key : std::string(desc)) + ": added '" + key + "'",
      });
      return true;
    }
    return false;
  }
  case ActionType::ReplaceIfDefault: {
    if (parent.is_object() && parent.contains(key)) {
      const auto& val = parent[key];
      if (JsonEquals(val, rule.expected_value)) {
        parent[key] = rule.to_value;
        changes.push_back({
            .file = "config.json",
            .description = desc_prefix + ": upgraded legacy default " +
                           FormatJson(rule.expected_value) + " to " + FormatJson(rule.to_value),
        });
        return true;
      }
    }
    return false;
  }
  default:
    return false;
  }
}

bool WalkAndApply(glz::json_t& node, std::span<const std::string_view> segments,
                  std::string_view desc, const MigrationRule& rule,
                  std::vector<MigrationChange>& changes) {
  if (segments.empty()) {
    return false;
  }

  const auto seg = segments[0];
  const bool is_leaf = (segments.size() == 1);

  if (is_leaf) {
    if (node.is_object()) {
      return ApplyActionToTarget(node, std::string(seg), desc, rule, changes);
    }
    return false;
  }

  const auto sub_segments = segments.subspan(1);
  bool any_changed = false;

  if (seg == "*") {
    if (node.is_object()) {
      for (auto& [child_key, child_value] : node.get_object()) {
        const std::string child_desc =
            desc.empty() ? child_key : std::string(desc) + "." + child_key;
        if (WalkAndApply(child_value, sub_segments, child_desc, rule, changes)) {
          any_changed = true;
        }
      }
    } else if (node.is_array()) {
      auto& array = node.get_array();
      for (std::size_t idx = 0; idx < array.size(); ++idx) {
        auto& child = array[idx];
        std::string child_id;
        if (child.is_object() && child.contains("id") && child["id"].is_string()) {
          child_id = child["id"].get<std::string>();
        } else {
          child_id = std::to_string(idx);
        }
        const std::string child_desc =
            desc.empty() ? "[" + child_id + "]" : std::string(desc) + "[" + child_id + "]";
        if (WalkAndApply(child, sub_segments, child_desc, rule, changes)) {
          any_changed = true;
        }
      }
    }
  } else if (node.is_object() && node.contains(seg)) {
    const std::string child_desc =
        desc.empty() ? std::string(seg) : std::string(desc) + "." + std::string(seg);
    if (WalkAndApply(node[seg], sub_segments, child_desc, rule, changes)) {
      any_changed = true;
    }
  }

  return any_changed;
}

bool HasOutdatedIniKeys(const fcitx::RawConfig& conf) {
  static constexpr std::string_view kOutdatedKeys[] = {
      "SceneMenuKey",
      "AsrMenuKey",
      "HoldActivationDelay",
  };
  bool found = false;
  conf.visitSubItems(
      [&](const fcitx::RawConfig& node, const std::string& /*path*/) {
        for (const auto key : kOutdatedKeys) {
          if (node.name() == key) {
            found = true;
            return false;
          }
        }
        return true;
      },
      "", true);
  return found;
}

bool ApplyIniRule(fcitx::RawConfig& root, const MigrationRule& rule,
                  std::vector<MigrationChange>& changes) {
  std::vector<std::string> matching_paths;
  root.visitSubItems(
      [&](const fcitx::RawConfig& node, const std::string& path) {
        if (rule.path.find('/') != std::string_view::npos) {
          if (path == rule.path) {
            matching_paths.push_back(path);
          }
        } else {
          if (node.name() == rule.path) {
            matching_paths.push_back(path);
          }
        }
        return true;
      },
      "", true);

  if (matching_paths.empty()) {
    return false;
  }

  bool changed = false;
  for (const auto& path : matching_paths) {
    const auto slash = path.rfind('/');
    fcitx::RawConfig* parent = nullptr;
    std::string key;
    if (slash != std::string::npos) {
      const auto parent_node = root.get(path.substr(0, slash));
      if (parent_node != nullptr) {
        parent = parent_node.get();
      }
      key = path.substr(slash + 1);
    } else {
      parent = &root;
      key = path;
    }
    if (parent == nullptr) {
      continue;
    }

    if (rule.action == ActionType::IniReplaceKey) {
      const auto old_node = parent->get(key);
      if (old_node != nullptr) {
        const std::string new_key(rule.new_key);
        if (parent->get(new_key) == nullptr) {
          auto new_node = parent->get(new_key, true);
          *new_node = *old_node;
          changes.push_back({
              .file = "vinput.conf",
              .description = "replaced key '" + std::string(rule.path) + "' with '" + new_key + "'",
          });
        } else {
          changes.push_back({
              .file = "vinput.conf",
              .description = "removed obsolete duplicate key '" + std::string(rule.path) + "'",
          });
        }
        parent->remove(key);
        changed = true;
      }
    } else if (rule.action == ActionType::IniRemoveKey) {
      if (parent->remove(key)) {
        changes.push_back({
            .file = "vinput.conf",
            .description = "removed obsolete key '" + std::string(rule.path) + "'",
        });
        changed = true;
      }
    }
  }
  return changed;
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

  std::filesystem::path real_config_path;
  if (!vinput::file::ResolveSymlinkPath(report.config_file, &real_config_path, &report.error)) {
    return report;
  }
  std::filesystem::path real_conf_path;
  if (!vinput::file::ResolveSymlinkPath(report.conf_file, &real_conf_path, &report.error)) {
    return report;
  }

  std::error_code ec;
  const bool config_exists = std::filesystem::is_regular_file(real_config_path, ec);
  const bool conf_exists = std::filesystem::is_regular_file(real_conf_path, ec);

  if (!config_exists && !conf_exists) {
    report.needed = false;
    report.applied = false;
    return report;
  }

  std::string raw_config;
  glz::json_t config_data;
  if (config_exists) {
    auto read_res = vinput::file::ReadTextFile(real_config_path);
    if (!read_res) {
      report.error = read_res.error();
      return report;
    }
    raw_config = std::move(*read_res);
    if (bool(glz::read<kMigrationReadOpts>(config_data, raw_config))) {
      report.error = "Failed to parse " + real_config_path.string() + ": invalid JSON";
      return report;
    }
  }

  std::string raw_conf;
  fcitx::RawConfig conf_raw_config;
  if (conf_exists) {
    auto read_res = vinput::file::ReadTextFile(real_conf_path);
    if (!read_res) {
      report.error = read_res.error();
      return report;
    }
    raw_conf = std::move(*read_res);
    fcitx::readAsIni(conf_raw_config, real_conf_path.string());
  }

  const auto max_rule_ver = GetMaxRuleVersion();

  // Fast-Path: if config version constraint satisfies host version, meets max rule version, and INI
  // has no outdated keys
  bool config_satisfies = true;
  if (config_exists) {
    if (config_data.is_object() && config_data.contains("version") &&
        config_data["version"].is_string() && !config_data.contains("vinput_version")) {
      const auto ver_str = config_data["version"].get<std::string>();
      const auto lower_bound = ParseConfigLowerBoundVersion(ver_str);
      config_satisfies = lower_bound.has_value() && *lower_bound >= max_rule_ver &&
                         vinput::version::Satisfies(vinput::version::CurrentHostVersion(), ver_str);
    } else {
      config_satisfies = false;
    }
  }

  const bool conf_has_outdated = conf_exists && HasOutdatedIniKeys(conf_raw_config);

  if (config_satisfies && !conf_has_outdated) {
    report.needed = false;
    report.applied = false;
    return report;
  }

  // Slow-Path: execute table-driven rules
  vinput::version::SemanticVersion config_ver{};
  bool is_unversioned = true;
  if (config_exists && config_data.is_object()) {
    if (config_data.contains("version") && config_data["version"].is_string()) {
      const auto parsed = ParseConfigLowerBoundVersion(config_data["version"].get<std::string>());
      if (parsed.has_value()) {
        config_ver = *parsed;
        is_unversioned = false;
      }
    }
  }

  const auto& rules = GetRules();
  bool config_changed = false;
  bool conf_changed = false;

  if (config_exists && config_data.is_object()) {
    for (const auto& rule : rules) {
      if (rule.target_file != "config.json") {
        continue;
      }
      const auto rule_ver_opt = vinput::version::Parse(rule.version);
      if (!rule_ver_opt.has_value()) {
        continue;
      }
      if (is_unversioned || config_ver < *rule_ver_opt) {
        const auto segments = vinput::str::Split(rule.path, '.');
        if (WalkAndApply(config_data, segments, "", rule, report.changes)) {
          config_changed = true;
        }
      }
    }

    if (config_data.contains("vinput_version")) {
      EraseMember(config_data, "vinput_version");
      config_changed = true;
      report.changes.push_back({
          .file = "config.json",
          .description = "removed legacy 'vinput_version' field",
      });
    }

    const std::string target_ver = GetTargetVersionConstraint();
    if (!config_data.contains("version") || !config_data["version"].is_string() ||
        config_data["version"].get<std::string>() != target_ver) {
      config_data["version"] = glz::json_t(target_ver);
      config_changed = true;
      report.changes.push_back({
          .file = "config.json",
          .description = "version: standardized constraint to '" + target_ver + "'",
      });
    }
  }

  if (conf_exists) {
    for (const auto& rule : rules) {
      if (rule.target_file != "vinput.conf") {
        continue;
      }
      if (ApplyIniRule(conf_raw_config, rule, report.changes)) {
        conf_changed = true;
      }
    }
  }

  report.needed = !report.changes.empty() || config_changed || conf_changed;
  if (!report.needed || dry_run) {
    report.applied = false;
    return report;
  }

  const std::string ts = GenerateTimestamp();
  if (config_changed && config_exists) {
    const auto backup_dir = real_config_path.parent_path() / "backups";
    std::filesystem::create_directories(backup_dir, ec);
    const auto backup_file = backup_dir / (real_config_path.filename().string() + ".bak." + ts);
    if (auto res = vinput::file::AtomicWriteTextFile(backup_file, raw_config); !res) {
      report.error =
          "Failed to create backup for " + real_config_path.string() + ": " + res.error();
      return report;
    }
    report.config_backup = backup_file;

    const std::string new_json_str = FormatPrettyJson(config_data) + "\n";
    if (auto res = vinput::file::AtomicWriteTextFile(real_config_path, new_json_str); !res) {
      report.error = "Failed to write migrated " + real_config_path.string() + ": " + res.error();
      return report;
    }
  }

  if (conf_changed && conf_exists) {
    const auto backup_dir = real_conf_path.parent_path() / "backups";
    std::filesystem::create_directories(backup_dir, ec);
    const auto backup_file = backup_dir / (real_conf_path.filename().string() + ".bak." + ts);
    if (auto res = vinput::file::AtomicWriteTextFile(backup_file, raw_conf); !res) {
      report.error = "Failed to create backup for " + real_conf_path.string() + ": " + res.error();
      return report;
    }
    report.conf_backup = backup_file;

    if (!fcitx::safeSaveAsIni(conf_raw_config, real_conf_path.string())) {
      report.error = "Failed to write migrated " + real_conf_path.string();
      return report;
    }
  }

  report.applied = true;
  return report;
}

} // namespace vinput::migration
