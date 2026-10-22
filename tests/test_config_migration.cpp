#include <cstdlib>
#include <filesystem>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <iostream>
#include <string>
#include <system_error>

#include "common/config/config_migration.h"
#include "common/json/json_access.h"
#include "common/utils/file_utils.h"

namespace {

void ExpectTrue(bool condition, const std::string& msg = "") {
  if (!condition) {
    std::cerr << "ExpectTrue failed! " << msg << "\n";
    std::abort();
  }
}

void ExpectEq(const std::string& actual, const std::string& expected, const std::string& msg = "") {
  if (actual != expected) {
    std::cerr << "ExpectEq failed: expected '" << expected << "', got '" << actual << "' (" << msg
              << ")\n";
    std::abort();
  }
}

void TestFastPath() {
  const auto tmp_dir = std::filesystem::temp_directory_path() / "vinput_test_mig_fast";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto config_path = tmp_dir / "config.json";
  const auto conf_path = tmp_dir / "vinput.conf";

  glz::json_t valid_cfg = glz::json_t(glz::json_t::object_t{});
  valid_cfg["version"] = ">=2.4.0";
  valid_cfg["asr"]["vad"]["threshold"] = 0.35;
  ExpectTrue(
      vinput::file::AtomicWriteTextFile(config_path, vinput::json::ToText(valid_cfg)).has_value());

  const std::string valid_conf = "[Trigger]\nMenuKey=Alt_R\n";
  ExpectTrue(vinput::file::AtomicWriteTextFile(conf_path, valid_conf).has_value());

  const auto report = vinput::migration::RunConfigMigration(false, config_path, conf_path);
  ExpectTrue(report.error.empty(), report.error);
  ExpectTrue(!report.needed, "Fast-path should detect needed == false");
  ExpectTrue(!report.applied, "Fast-path should not apply changes");
  ExpectTrue(report.changes.empty(), "Fast-path changes should be empty");

  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
}

void TestDryRunAndApply() {
  const auto tmp_dir = std::filesystem::temp_directory_path() / "vinput_test_mig_dry_apply";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto config_path = tmp_dir / "config.json";
  const auto conf_path = tmp_dir / "vinput.conf";

  const std::string legacy_config_str = R"({
  "version": 1,
  "global": {
    "capture_device": "default",
    "default_language": "zh_CN"
  },
  "asr": {
    "active_provider": "sherpa-onnx",
    "vad": {
      "threshold": 0.45,
      "min_speech_duration": 0.15,
      "min_silence_duration": 0.5,
      "speech_pad_ms": 30
    }
  },
  "scenes": {
    "definitions": [
      {
        "id": "code",
        "candidate_count": 3,
        "show_raw": true
      }
    ]
  },
  "llm": {
    "adaptors": [
      {
        "id": "cjk-trim",
        "command": "python3"
      }
    ]
  }
})";

  const std::string legacy_conf_str =
      "[Trigger]\nSceneMenuKey=Alt+Shift+V\nAsrMenuKey=Control+Shift+V\nHoldActivationDelay=200\n";

  ExpectTrue(vinput::file::AtomicWriteTextFile(config_path, legacy_config_str).has_value());
  ExpectTrue(vinput::file::AtomicWriteTextFile(conf_path, legacy_conf_str).has_value());

  // 1. Dry run
  const auto dry_report = vinput::migration::RunConfigMigration(true, config_path, conf_path);
  ExpectTrue(dry_report.error.empty(), dry_report.error);
  ExpectTrue(dry_report.needed, "Dry-run should report needed == true");
  ExpectTrue(!dry_report.applied, "Dry-run should report applied == false");
  ExpectTrue(dry_report.changes.size() >= 6, "Dry-run should contain at least 6 changes");

  // Verify files were NOT modified on disk during dry-run
  auto r_back = vinput::file::ReadTextFile(config_path);
  ExpectTrue(r_back.has_value(), r_back ? "" : r_back.error());
  std::string read_back = *r_back;
  glz::json_t unchanged_cfg;
  ExpectTrue(!bool(glz::read_json(unchanged_cfg, read_back)), "dry-run config stays valid JSON");
  const auto* definitions = vinput::json::Find(unchanged_cfg["scenes"], "definitions");
  ExpectTrue(definitions != nullptr && definitions->is_array() &&
                 !definitions->get_array().empty() &&
                 definitions->get_array().front().contains("candidate_count"),
             "dry-run must not rename candidate_count");

  // 2. Apply migration
  const auto apply_report = vinput::migration::RunConfigMigration(false, config_path, conf_path);
  ExpectTrue(apply_report.error.empty(), apply_report.error);
  ExpectTrue(apply_report.needed, "Apply should report needed == true");
  ExpectTrue(apply_report.applied, "Apply should report applied == true");
  ExpectTrue(std::filesystem::exists(apply_report.config_backup), "Config backup should exist");
  ExpectTrue(std::filesystem::exists(apply_report.conf_backup), "Conf backup should exist");

  // 3. Verify in-place modified content
  auto r_cfg = vinput::file::ReadTextFile(config_path);
  ExpectTrue(r_cfg.has_value(), r_cfg ? "" : r_cfg.error());
  std::string migrated_config_str = *r_cfg;
  glz::json_t migrated_cfg;
  ExpectTrue(!bool(glz::read_json(migrated_cfg, migrated_config_str)),
             "migrated config is valid JSON");

  const auto* migrated_definitions = vinput::json::Find(migrated_cfg["scenes"], "definitions");
  ExpectTrue(migrated_definitions != nullptr && migrated_definitions->is_array() &&
                 !migrated_definitions->get_array().empty(),
             "definitions array preserved");
  const auto& sc_def = migrated_definitions->get_array().front();
  ExpectTrue(sc_def.contains("count") && !sc_def.contains("candidate_count"),
             "candidate_count -> count");
  ExpectEq(std::to_string(vinput::json::Int(sc_def, "count")), "3");
  ExpectTrue(sc_def.contains("raw_cand") && !sc_def.contains("show_raw"), "show_raw -> raw_cand");
  ExpectTrue(vinput::json::Bool(sc_def, "raw_prev"), "raw_prev should be ensured true");

  ExpectTrue(migrated_cfg["llm"].contains("adapters") && !migrated_cfg["llm"].contains("adaptors"),
             "adaptors -> adapters");
  ExpectTrue(!migrated_cfg["global"].contains("default_language"), "default_language removed");

  const auto& vad = migrated_cfg["asr"]["vad"];
  ExpectTrue(std::abs(vinput::json::Number(vad, "threshold") - 0.35) < 1e-4,
             "threshold upgraded to 0.35");
  ExpectTrue(std::abs(vinput::json::Number(vad, "min_speech_duration") - 0.10) < 1e-4,
             "min_speech_duration upgraded to 0.10");
  ExpectTrue(std::abs(vinput::json::Number(vad, "min_silence_duration") - 1.2) < 1e-4,
             "min_silence_duration upgraded to 1.2");
  ExpectEq(std::to_string(vinput::json::Int(vad, "speech_pad_ms")), "500",
           "speech_pad_ms upgraded to 500");
  ExpectEq(vinput::json::String(migrated_cfg, "version"), ">=2.4.0",
           "version stamped with >=2.4.0");

  std::string migrated_conf_str;
  auto r_conf = vinput::file::ReadTextFile(conf_path);
  ExpectTrue(r_conf.has_value(), r_conf ? "" : r_conf.error());
  migrated_conf_str = *r_conf;
  ExpectTrue(migrated_conf_str.find("MenuKey=Alt+Shift+V") != std::string::npos, "MenuKey updated");
  ExpectTrue(migrated_conf_str.find("SceneMenuKey") == std::string::npos, "SceneMenuKey removed");
  ExpectTrue(migrated_conf_str.find("AsrMenuKey") == std::string::npos, "AsrMenuKey removed");
  ExpectTrue(migrated_conf_str.find("HoldActivationDelay") == std::string::npos,
             "HoldActivationDelay removed");

  // 4. Test Idempotency (run again should detect needed == false)
  const auto idemp_report = vinput::migration::RunConfigMigration(false, config_path, conf_path);
  ExpectTrue(idemp_report.error.empty(), idemp_report.error);
  ExpectTrue(!idemp_report.needed, "Second migration run should report needed == false");
  ExpectTrue(!idemp_report.applied, "Second migration run should report applied == false");

  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
}

void TestSymlinkPreservation() {
  const auto tmp_dir = std::filesystem::temp_directory_path() / "vinput_test_mig_symlink";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto real_dir = tmp_dir / "dotfiles";
  const auto sym_dir = tmp_dir / "config";
  std::filesystem::create_directories(real_dir);
  std::filesystem::create_directories(sym_dir);

  const auto real_config = real_dir / "config.json";
  const std::string legacy_cfg = R"({"version": 1, "llm": {"adaptors": [{"id": "foo"}]}})";
  ExpectTrue(vinput::file::AtomicWriteTextFile(real_config, legacy_cfg).has_value());

  const auto sym_config = sym_dir / "config.json";
  std::filesystem::create_symlink(real_config, sym_config);
  ExpectTrue(std::filesystem::is_symlink(sym_config));

  const auto report =
      vinput::migration::RunConfigMigration(false, sym_config, sym_dir / "nonexistent.conf");
  ExpectTrue(report.error.empty(), report.error);
  ExpectTrue(report.applied, "Migration should apply on symlink");

  // Symlink must remain a symlink
  ExpectTrue(std::filesystem::is_symlink(sym_config), "Symlink was overwritten by regular file!");
  std::error_code ec;
  ExpectTrue(std::filesystem::canonical(sym_config, ec) ==
                 std::filesystem::canonical(real_config, ec),
             "Symlink should still point to real config");

  // Backup must be in real_dir/backups
  ExpectTrue(std::filesystem::is_directory(real_dir / "backups"),
             "Backup directory should be under real target");

  auto r_updated = vinput::file::ReadTextFile(real_config);
  ExpectTrue(r_updated.has_value(), r_updated ? "" : r_updated.error());
  std::string updated_raw = *r_updated;
  glz::json_t updated;
  ExpectTrue(!bool(glz::read_json(updated, updated_raw)), "symlink target is valid JSON");
  ExpectTrue(updated["llm"].contains("adapters") && !updated["llm"].contains("adaptors"),
             "Real config was migrated");

  std::filesystem::remove_all(tmp_dir, ec);
}

void TestDefaultTemplateFastPath() {
  const auto default_cfg_path =
      std::filesystem::path(CMAKE_SOURCE_DIR_STR) / "data" / "default-config.json";
  if (!std::filesystem::exists(default_cfg_path)) {
    return;
  }
  const auto report = vinput::migration::RunConfigMigration(true, default_cfg_path, "/dev/null");
  ExpectTrue(report.error.empty(), report.error);
  ExpectTrue(!report.needed, "default-config.json dry-run should report needed == false");
}

void TestVersionConstraintAndCustomizationPreserved() {
  const auto tmp_dir = std::filesystem::temp_directory_path() / "vinput_test_mig_custom_preserved";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto config_path = tmp_dir / "config.json";
  const auto conf_path = tmp_dir / "vinput.conf";

  // config.json already has version >=2.4.0 and intentionally custom threshold 0.45
  const glz::json_t cfg = {
      {"version", ">=2.4.0"},
      {"asr",
       {{"vad",
         {{"threshold", 0.45},
          {"min_speech_duration", 0.10},
          {"min_silence_duration", 1.2},
          {"speech_pad_ms", 500}}}}},
  };
  ExpectTrue(
      vinput::file::AtomicWriteTextFile(config_path, vinput::json::ToText(cfg) + "\n").has_value());

  // vinput.conf has obsolete HoldActivationDelay
  const std::string initial_conf = "TriggerKey=Alt_R\nHoldActivationDelay=200\n";
  ExpectTrue(vinput::file::AtomicWriteTextFile(conf_path, initial_conf).has_value());

  const auto report = vinput::migration::RunConfigMigration(false, config_path, conf_path);
  ExpectTrue(report.error.empty(), report.error);
  ExpectTrue(report.needed, "Migration should be needed due to obsolete INI key");
  ExpectTrue(report.applied, "Migration should be applied");

  // config.json threshold 0.45 must NOT be overwritten by migration rules
  auto r_cfg2 = vinput::file::ReadTextFile(config_path);
  ExpectTrue(r_cfg2.has_value(), r_cfg2 ? "" : r_cfg2.error());
  std::string read_cfg_str = *r_cfg2;
  glz::json_t migrated_cfg;
  ExpectTrue(!bool(glz::read_json(migrated_cfg, read_cfg_str)), "config stays valid JSON");
  ExpectTrue(std::abs(vinput::json::Number(migrated_cfg["asr"]["vad"], "threshold") - 0.45) < 1e-4,
             "Custom threshold 0.45 must be preserved");

  // vinput.conf must have HoldActivationDelay removed
  auto r_conf2 = vinput::file::ReadTextFile(conf_path);
  ExpectTrue(r_conf2.has_value(), r_conf2 ? "" : r_conf2.error());
  const std::string read_conf_str = *r_conf2;
  ExpectTrue(read_conf_str.find("HoldActivationDelay") == std::string::npos,
             "HoldActivationDelay must be removed from vinput.conf");

  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
}

void TestUndefinedFieldsAndIntegrityPreserved() {
  const auto tmp_dir = std::filesystem::temp_directory_path() / "vinput_test_mig_undefined_fields";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto config_path = tmp_dir / "config.json";
  const auto conf_path = tmp_dir / "vinput.conf";

  const glz::json_t valid_cfg = {
      {"version", ">=2.4.0"},
  };
  ExpectTrue(vinput::file::AtomicWriteTextFile(config_path, vinput::json::ToText(valid_cfg) + "\n")
                 .has_value());

  const std::string conf_with_custom = "[Trigger]\n"
                                       "TriggerKey=Alt_R\n"
                                       "SceneMenuKey=Alt+Shift+V\n"
                                       "AsrMenuKey=Control+Shift+V\n"
                                       "CustomTriggerOption=CustomValue\n"
                                       "\n"
                                       "[CustomSection]\n"
                                       "CustomKey=CustomVal\n"
                                       "AnotherKey=AnotherVal\n";
  ExpectTrue(vinput::file::AtomicWriteTextFile(conf_path, conf_with_custom).has_value());

  const auto report = vinput::migration::RunConfigMigration(false, config_path, conf_path);
  ExpectTrue(report.error.empty(), report.error);
  ExpectTrue(report.needed, "Migration should be needed");
  ExpectTrue(report.applied, "Migration should be applied");

  auto r_conf3 = vinput::file::ReadTextFile(conf_path);
  ExpectTrue(r_conf3.has_value(), r_conf3 ? "" : r_conf3.error());
  const std::string read_conf_str = *r_conf3;

  // Replaced / removed keys
  ExpectTrue(read_conf_str.find("MenuKey=Alt+Shift+V") != std::string::npos,
             "MenuKey must be present after renaming SceneMenuKey");
  ExpectTrue(read_conf_str.find("SceneMenuKey") == std::string::npos,
             "SceneMenuKey must be removed");
  ExpectTrue(read_conf_str.find("AsrMenuKey") == std::string::npos, "AsrMenuKey must be removed");

  // Undefined and custom keys/sections must be fully preserved
  ExpectTrue(read_conf_str.find("TriggerKey=Alt_R") != std::string::npos,
             "TriggerKey must be preserved");
  ExpectTrue(read_conf_str.find("CustomTriggerOption=CustomValue") != std::string::npos,
             "CustomTriggerOption must be preserved");
  ExpectTrue(read_conf_str.find("[CustomSection]") != std::string::npos,
             "[CustomSection] must be preserved");
  ExpectTrue(read_conf_str.find("CustomKey=CustomVal") != std::string::npos,
             "CustomKey must be preserved");
  ExpectTrue(read_conf_str.find("AnotherKey=AnotherVal") != std::string::npos,
             "AnotherKey must be preserved");

  // Idempotency check
  const auto second_report = vinput::migration::RunConfigMigration(false, config_path, conf_path);
  ExpectTrue(second_report.error.empty(), second_report.error);
  ExpectTrue(!second_report.needed, "Second run should report needed == false");

  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
}

} // namespace

int main() {
  TestFastPath();
  TestDryRunAndApply();
  TestSymlinkPreservation();
  TestDefaultTemplateFastPath();
  TestVersionConstraintAndCustomizationPreserved();
  TestUndefinedFieldsAndIntegrityPreserved();
  std::cout << "All test_config_migration tests passed!\n";
  return 0;
}
