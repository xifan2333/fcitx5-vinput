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
#include <tl/expected.hpp>
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

      // v2.4.0: Remove obsolete auto_start from llm.adapters
      {"2.4.0", "config.json", ActionType::Remove, "llm.adapters.*.auto_start"},
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

bool ApplyRenameAction(glz::json_t& parent, const std::string& key, std::string_view desc_prefix,
                       std::string_view new_key_view, std::vector<MigrationChange>& changes) {
  if (!parent.contains(key)) {
    return false;
  }
  const std::string new_key(new_key_view);
  auto& object = parent.get_object();
  if (!parent.contains(new_key)) {
    object.emplace(new_key, std::move(object[key]));
    object.erase(key);
    changes.push_back({
        .file = "config.json",
        .description = std::string(desc_prefix) + ": renamed '" + key + "' to '" + new_key + "'",
    });
  } else {
    EraseMember(parent, key);
    changes.push_back({
        .file = "config.json",
        .description = std::string(desc_prefix) + ": removed obsolete duplicate key '" + key + "'",
    });
  }
  return true;
}

bool ApplyRemoveAction(glz::json_t& parent, const std::string& key, std::string_view target_desc,
                       std::vector<MigrationChange>& changes) {
  if (!parent.contains(key)) {
    return false;
  }
  EraseMember(parent, key);
  changes.push_back({
      .file = "config.json",
      .description = std::string(target_desc) + ": removed obsolete field '" + key + "'",
  });
  return true;
}

bool ApplyEnsureAction(glz::json_t& parent, const std::string& key, std::string_view target_desc,
                       const glz::json_t& default_val, std::vector<MigrationChange>& changes) {
  if (parent.contains(key)) {
    return false;
  }
  parent[key] = default_val;
  changes.push_back({
      .file = "config.json",
      .description = std::string(target_desc) + ": added '" + key + "'",
  });
  return true;
}

bool ApplyReplaceIfDefaultAction(glz::json_t& parent, const std::string& key,
                                 std::string_view desc_prefix, const glz::json_t& expected_val,
                                 const glz::json_t& to_val, std::vector<MigrationChange>& changes) {
  if (!parent.contains(key)) {
    return false;
  }
  const auto& val = parent[key];
  if (!JsonEquals(val, expected_val)) {
    return false;
  }
  parent[key] = to_val;
  changes.push_back({
      .file = "config.json",
      .description = std::string(desc_prefix) + ": upgraded legacy default " +
                     FormatJson(expected_val) + " to " + FormatJson(to_val),
  });
  return true;
}

bool ApplyActionToTarget(glz::json_t& parent, const std::string& key, std::string_view desc,
                         const MigrationRule& rule, std::vector<MigrationChange>& changes) {
  if (!parent.is_object()) {
    return false;
  }

  const std::string desc_prefix = desc.empty() ? key : std::string(desc) + "." + key;
  const std::string target_desc = desc.empty() ? key : std::string(desc);

  switch (rule.action) {
  case ActionType::Rename:
    return ApplyRenameAction(parent, key, desc_prefix, rule.new_key, changes);
  case ActionType::Remove:
    return ApplyRemoveAction(parent, key, target_desc, changes);
  case ActionType::Ensure:
    return ApplyEnsureAction(parent, key, target_desc, rule.default_value, changes);
  case ActionType::ReplaceIfDefault:
    return ApplyReplaceIfDefaultAction(parent, key, desc_prefix, rule.expected_value, rule.to_value,
                                       changes);
  default:
    return false;
  }
}

bool WalkAndApply(glz::json_t& node, std::span<const std::string_view> segments,
                  std::string_view desc, const MigrationRule& rule,
                  std::vector<MigrationChange>& changes);

bool WalkWildcardObject(glz::json_t& node, std::span<const std::string_view> sub_segments,
                        std::string_view desc, const MigrationRule& rule,
                        std::vector<MigrationChange>& changes) {
  if (!node.is_object()) {
    return false;
  }
  bool any_changed = false;
  for (auto& [child_key, child_value] : node.get_object()) {
    const std::string child_desc = desc.empty() ? child_key : std::string(desc) + "." + child_key;
    if (WalkAndApply(child_value, sub_segments, child_desc, rule, changes)) {
      any_changed = true;
    }
  }
  return any_changed;
}

bool WalkWildcardArray(glz::json_t& node, std::span<const std::string_view> sub_segments,
                       std::string_view desc, const MigrationRule& rule,
                       std::vector<MigrationChange>& changes) {
  if (!node.is_array()) {
    return false;
  }
  bool any_changed = false;
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
  return any_changed;
}

bool WalkObjectProperty(glz::json_t& node, std::string_view seg,
                        std::span<const std::string_view> sub_segments, std::string_view desc,
                        const MigrationRule& rule, std::vector<MigrationChange>& changes) {
  if (!node.is_object() || !node.contains(seg)) {
    return false;
  }
  const std::string child_desc =
      desc.empty() ? std::string(seg) : std::string(desc) + "." + std::string(seg);
  return WalkAndApply(node[seg], sub_segments, child_desc, rule, changes);
}

bool WalkAndApply(glz::json_t& node, std::span<const std::string_view> segments,
                  std::string_view desc, const MigrationRule& rule,
                  std::vector<MigrationChange>& changes) {
  if (segments.empty()) {
    return false;
  }

  const auto seg = segments[0];
  if (segments.size() == 1) {
    if (node.is_object()) {
      return ApplyActionToTarget(node, std::string(seg), desc, rule, changes);
    }
    return false;
  }

  const auto sub_segments = segments.subspan(1);
  if (seg == "*") {
    if (node.is_object()) {
      return WalkWildcardObject(node, sub_segments, desc, rule, changes);
    }
    if (node.is_array()) {
      return WalkWildcardArray(node, sub_segments, desc, rule, changes);
    }
    return false;
  }

  return WalkObjectProperty(node, seg, sub_segments, desc, rule, changes);
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

std::vector<std::string> CollectMatchingIniPaths(const fcitx::RawConfig& root,
                                                 std::string_view target_path) {
  std::vector<std::string> matching_paths;
  const bool has_slash = (target_path.find('/') != std::string_view::npos);
  root.visitSubItems(
      [&](const fcitx::RawConfig& node, const std::string& path) {
        if (has_slash) {
          if (path == target_path) {
            matching_paths.push_back(path);
          }
        } else if (node.name() == target_path) {
          matching_paths.push_back(path);
        }
        return true;
      },
      "", true);
  return matching_paths;
}

std::pair<fcitx::RawConfig*, std::string> ResolveIniParentAndKey(fcitx::RawConfig& root,
                                                                 std::string_view path) {
  const auto slash = path.rfind('/');
  if (slash == std::string_view::npos) {
    return {&root, std::string(path)};
  }
  const auto parent_node = root.get(std::string(path.substr(0, slash)));
  return {parent_node != nullptr ? parent_node.get() : nullptr,
          std::string(path.substr(slash + 1))};
}

bool ApplySingleIniRule(fcitx::RawConfig& parent, const std::string& key, const MigrationRule& rule,
                        std::vector<MigrationChange>& changes) {
  if (rule.action == ActionType::IniReplaceKey) {
    const auto old_node = parent.get(key);
    if (old_node == nullptr) {
      return false;
    }
    const std::string new_key(rule.new_key);
    if (parent.get(new_key) == nullptr) {
      auto new_node = parent.get(new_key, true);
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
    parent.remove(key);
    return true;
  }

  if (rule.action == ActionType::IniRemoveKey) {
    if (parent.remove(key)) {
      changes.push_back({
          .file = "vinput.conf",
          .description = "removed obsolete key '" + std::string(rule.path) + "'",
      });
      return true;
    }
    return false;
  }

  return false;
}

bool ApplyIniRule(fcitx::RawConfig& root, const MigrationRule& rule,
                  std::vector<MigrationChange>& changes) {
  const auto matching_paths = CollectMatchingIniPaths(root, rule.path);
  if (matching_paths.empty()) {
    return false;
  }

  bool changed = false;
  for (const auto& path : matching_paths) {
    auto [parent, key] = ResolveIniParentAndKey(root, path);
    if (parent != nullptr && ApplySingleIniRule(*parent, key, rule, changes)) {
      changed = true;
    }
  }
  return changed;
}

struct LoadedInputs {
  std::filesystem::path real_config_path;
  std::filesystem::path real_conf_path;
  bool config_exists{false};
  bool conf_exists{false};
  std::string raw_config;
  std::string raw_conf;
  glz::json_t config_data;
  fcitx::RawConfig conf_raw_config;
};

std::optional<LoadedInputs> ResolveAndLoadInputs(MigrationReport& report) {
  LoadedInputs inputs;
  if (!vinput::file::ResolveSymlinkPath(report.config_file, &inputs.real_config_path,
                                        &report.error)) {
    return std::nullopt;
  }
  if (!vinput::file::ResolveSymlinkPath(report.conf_file, &inputs.real_conf_path, &report.error)) {
    return std::nullopt;
  }

  std::error_code ec;
  inputs.config_exists = std::filesystem::is_regular_file(inputs.real_config_path, ec);
  inputs.conf_exists = std::filesystem::is_regular_file(inputs.real_conf_path, ec);

  if (!inputs.config_exists && !inputs.conf_exists) {
    report.needed = false;
    report.applied = false;
    return inputs;
  }

  if (inputs.config_exists) {
    auto read_res = vinput::file::ReadTextFile(inputs.real_config_path);
    if (!read_res) {
      report.error = read_res.error();
      return std::nullopt;
    }
    inputs.raw_config = std::move(*read_res);
    if (bool(glz::read<kMigrationReadOpts>(inputs.config_data, inputs.raw_config))) {
      report.error = "Failed to parse " + inputs.real_config_path.string() + ": invalid JSON";
      return std::nullopt;
    }
  }

  if (inputs.conf_exists) {
    auto read_res = vinput::file::ReadTextFile(inputs.real_conf_path);
    if (!read_res) {
      report.error = read_res.error();
      return std::nullopt;
    }
    inputs.raw_conf = std::move(*read_res);
    fcitx::readAsIni(inputs.conf_raw_config, inputs.real_conf_path.string());
  }

  return inputs;
}

bool IsFastPathSatisfied(const LoadedInputs& inputs,
                         const vinput::version::SemanticVersion& max_rule_ver) {
  if (inputs.conf_exists && HasOutdatedIniKeys(inputs.conf_raw_config)) {
    return false;
  }

  if (!inputs.config_exists) {
    return true;
  }

  const auto& cfg = inputs.config_data;
  if (!cfg.is_object() || !cfg.contains("version") || !cfg["version"].is_string() ||
      cfg.contains("vinput_version")) {
    return false;
  }

  const auto ver_str = cfg["version"].get<std::string>();
  const auto lower_bound = ParseConfigLowerBoundVersion(ver_str);
  return lower_bound.has_value() && *lower_bound >= max_rule_ver &&
         vinput::version::Satisfies(vinput::version::CurrentHostVersion(), ver_str);
}

bool ShouldApplyRule(const MigrationRule& rule,
                     const std::optional<vinput::version::SemanticVersion>& config_ver) {
  if (rule.target_file != "config.json") {
    return false;
  }
  const auto rule_ver_opt = vinput::version::Parse(rule.version);
  if (!rule_ver_opt.has_value()) {
    return false;
  }
  return !config_ver.has_value() || *config_ver < *rule_ver_opt;
}

bool ApplyRulesToJson(glz::json_t& config_data, const std::vector<MigrationRule>& rules,
                      const std::optional<vinput::version::SemanticVersion>& config_ver,
                      std::vector<MigrationChange>& changes) {
  bool changed = false;
  for (const auto& rule : rules) {
    if (ShouldApplyRule(rule, config_ver)) {
      const auto segments = vinput::str::Split(rule.path, '.');
      if (WalkAndApply(config_data, segments, "", rule, changes)) {
        changed = true;
      }
    }
  }
  return changed;
}

bool StandardizeVersionFields(glz::json_t& config_data, std::vector<MigrationChange>& changes) {
  bool changed = false;
  if (config_data.contains("vinput_version")) {
    EraseMember(config_data, "vinput_version");
    changed = true;
    changes.push_back({
        .file = "config.json",
        .description = "removed legacy 'vinput_version' field",
    });
  }

  const std::string target_ver = GetTargetVersionConstraint();
  const bool needs_ver_update = !config_data.contains("version") ||
                                !config_data["version"].is_string() ||
                                config_data["version"].get<std::string>() != target_ver;
  if (needs_ver_update) {
    config_data["version"] = glz::json_t(target_ver);
    changed = true;
    changes.push_back({
        .file = "config.json",
        .description = "version: standardized constraint to '" + target_ver + "'",
    });
  }
  return changed;
}

std::optional<vinput::version::SemanticVersion>
ExtractConfigVersion(const glz::json_t& config_data) {
  if (config_data.contains("version") && config_data["version"].is_string()) {
    return ParseConfigLowerBoundVersion(config_data["version"].get<std::string>());
  }
  return std::nullopt;
}

bool MigrateJsonConfig(glz::json_t& config_data, const std::vector<MigrationRule>& rules,
                       std::vector<MigrationChange>& changes) {
  if (!config_data.is_object()) {
    return false;
  }
  const auto config_ver = ExtractConfigVersion(config_data);
  const bool rules_changed = ApplyRulesToJson(config_data, rules, config_ver, changes);
  const bool ver_changed = StandardizeVersionFields(config_data, changes);
  return rules_changed || ver_changed;
}

bool MigrateIniConfig(fcitx::RawConfig& conf_raw, const std::vector<MigrationRule>& rules,
                      std::vector<MigrationChange>& changes) {
  bool conf_changed = false;
  for (const auto& rule : rules) {
    if (rule.target_file != "vinput.conf") {
      continue;
    }
    if (ApplyIniRule(conf_raw, rule, changes)) {
      conf_changed = true;
    }
  }
  return conf_changed;
}

tl::expected<std::filesystem::path, std::string>
CreateBackupFile(const std::filesystem::path& target_path, std::string_view raw_content,
                 std::string_view timestamp) {
  std::error_code ec;
  const auto backup_dir = target_path.parent_path() / "backups";
  std::filesystem::create_directories(backup_dir, ec);
  auto backup_file =
      backup_dir / (target_path.filename().string() + ".bak." + std::string(timestamp));
  auto res = vinput::file::AtomicWriteTextFile(backup_file, raw_content);
  if (!res) {
    return tl::make_unexpected("Failed to create backup for " + target_path.string() + ": " +
                               res.error());
  }
  return backup_file;
}

bool SaveMigratedConfig(const LoadedInputs& inputs, const glz::json_t& config_data,
                        std::string_view timestamp, MigrationReport& report) {
  auto backup_res = CreateBackupFile(inputs.real_config_path, inputs.raw_config, timestamp);
  if (!backup_res) {
    report.error = backup_res.error();
    return false;
  }
  report.config_backup = *backup_res;

  const std::string new_json_str = FormatPrettyJson(config_data) + "\n";
  if (auto res = vinput::file::AtomicWriteTextFile(inputs.real_config_path, new_json_str); !res) {
    report.error =
        "Failed to write migrated " + inputs.real_config_path.string() + ": " + res.error();
    return false;
  }
  return true;
}

bool SaveMigratedConf(const LoadedInputs& inputs, fcitx::RawConfig& conf_raw_config,
                      std::string_view timestamp, MigrationReport& report) {
  auto backup_res = CreateBackupFile(inputs.real_conf_path, inputs.raw_conf, timestamp);
  if (!backup_res) {
    report.error = backup_res.error();
    return false;
  }
  report.conf_backup = *backup_res;

  if (!fcitx::safeSaveAsIni(conf_raw_config, inputs.real_conf_path.string())) {
    report.error = "Failed to write migrated " + inputs.real_conf_path.string();
    return false;
  }
  return true;
}

void ResolveReportPaths(const std::filesystem::path& config_path,
                        const std::filesystem::path& conf_path, MigrationReport& report) {
  report.config_file = config_path.empty() ? vinput::path::CoreConfigPath() : config_path;
  report.conf_file = conf_path.empty() ? vinput::path::FcitxAddonConfigPath() : conf_path;
}

bool SaveMigratedFiles(LoadedInputs& inputs, bool config_changed, bool conf_changed,
                       MigrationReport& report) {
  const std::string ts = GenerateTimestamp();
  if (config_changed) {
    if (!SaveMigratedConfig(inputs, inputs.config_data, ts, report)) {
      return false;
    }
  }
  if (conf_changed) {
    if (!SaveMigratedConf(inputs, inputs.conf_raw_config, ts, report)) {
      return false;
    }
  }
  return true;
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
  ResolveReportPaths(config_path, conf_path, report);

  auto loaded_inputs = ResolveAndLoadInputs(report);
  if (!loaded_inputs.has_value()) {
    return report;
  }
  auto& inputs = *loaded_inputs;
  if (!inputs.config_exists && !inputs.conf_exists) {
    return report;
  }

  if (IsFastPathSatisfied(inputs, GetMaxRuleVersion())) {
    report.needed = false;
    report.applied = false;
    return report;
  }

  const auto& rules = GetRules();
  const bool config_changed =
      inputs.config_exists ? MigrateJsonConfig(inputs.config_data, rules, report.changes) : false;
  const bool conf_changed =
      inputs.conf_exists ? MigrateIniConfig(inputs.conf_raw_config, rules, report.changes) : false;

  const bool has_changes = !report.changes.empty() || config_changed || conf_changed;
  report.needed = has_changes;
  if (!has_changes || dry_run) {
    report.applied = false;
    return report;
  }

  if (!SaveMigratedFiles(inputs, config_changed, conf_changed, report)) {
    return report;
  }

  report.applied = true;
  return report;
}

} // namespace vinput::migration
