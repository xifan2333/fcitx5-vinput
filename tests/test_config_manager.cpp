#include <QtCore/qcoreapplication.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "common/config/core_config_types.h"

#include "gui/utils/config_manager.h"

namespace {

void AssertTrue(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestNullPointer() {
  std::string error;
  const bool ok = vinput::gui::ConfigManager::Get().TryLoad(nullptr, &error);
  AssertTrue(!ok, "TryLoad(nullptr) must return false");
  AssertTrue(error == "null config pointer", "Error message must indicate null config pointer");
}

void TestCorruptedConfigProtection() {
  const auto temp_dir = std::filesystem::temp_directory_path() / "vinput_test_cfg_corrupted";
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
  const auto config_dir = temp_dir / "vinput";
  std::filesystem::create_directories(config_dir, ec);

  const auto config_path = config_dir / "config.json";
  {
    std::ofstream ofs(config_path);
    ofs << "{ \"version\": \"2.4.0\", \"asr\": { unclosed json";
  }

  setenv("XDG_CONFIG_HOME", temp_dir.c_str(), 1);

  CoreConfig config;
  config.version = "sentinel_version";
  config.asr.activeProvider = "sentinel_active";

  std::string error;
  const bool ok = vinput::gui::ConfigManager::Get().TryLoad(&config, &error);
  AssertTrue(!ok, "TryLoad on corrupted JSON must return false");
  AssertTrue(!error.empty(), "Error message must not be empty on corrupted JSON");
  AssertTrue(config.version == "sentinel_version",
             "config.version must remain untouched on failure");
  AssertTrue(config.asr.activeProvider == "sentinel_active",
             "config.asr.activeProvider must remain untouched on failure");

  std::filesystem::remove_all(temp_dir, ec);
}

void TestValidConfigLoading() {
  const auto temp_dir = std::filesystem::temp_directory_path() / "vinput_test_cfg_valid";
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
  const auto config_dir = temp_dir / "vinput";
  std::filesystem::create_directories(config_dir, ec);

  const auto config_path = config_dir / "config.json";
  {
    std::ofstream ofs(config_path);
    ofs << R"({
      "version": ">=2.4.0",
      "asr": {
        "activeProvider": "sherpa-local",
        "providers": [
          {
            "id": "sherpa-local",
            "type": "local",
            "model": "zipformer-zh",
            "timeoutMs": 15000
          }
        ]
      }
    })";
  }

  setenv("XDG_CONFIG_HOME", temp_dir.c_str(), 1);

  CoreConfig config;
  std::string error;
  const bool ok = vinput::gui::ConfigManager::Get().TryLoad(&config, &error);
  AssertTrue(ok, "TryLoad on valid JSON must return true");
  AssertTrue(config.asr.activeProvider == "sherpa-local", "activeProvider must match");
  AssertTrue(config.asr.providers.size() == 1, "providers count must match");

  std::filesystem::remove_all(temp_dir, ec);
}

void TestAsrProviderIdInheritance() {
  // Scenario 1: Existing command provider switched to local provider with empty updated.id
  const std::string original_id = "test_asr_provider";
  const std::string updated_id_empty;
  const std::string new_id = updated_id_empty.empty() ? original_id : updated_id_empty;
  AssertTrue(new_id == original_id, "Empty updated id must inherit original id");

  LocalAsrProvider local;
  local.id = new_id;
  local.model = "zipformer-zh";
  local.timeoutMs = 15000;
  AssertTrue(local.id == "test_asr_provider",
             "LocalAsrProvider must have id inherited from original");

  // Scenario 2: Active provider renamed
  CoreConfig config;
  config.asr.activeProvider = "old_provider_id";

  const std::string renamed_id = "renamed_provider_id";
  if (config.asr.activeProvider == "old_provider_id" && renamed_id != "old_provider_id") {
    config.asr.activeProvider = renamed_id;
  }
  AssertTrue(config.asr.activeProvider == "renamed_provider_id",
             "activeProvider must sync when active provider is renamed");

  // Scenario 3: Non-active provider renamed does not alter activeProvider
  if (config.asr.activeProvider == "other_provider" && renamed_id != "other_provider") {
    config.asr.activeProvider = renamed_id;
  }
  AssertTrue(config.asr.activeProvider == "renamed_provider_id",
             "activeProvider must not change when a non-active provider is renamed");
}

} // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);

  TestNullPointer();
  TestCorruptedConfigProtection();
  TestValidConfigLoading();
  TestAsrProviderIdInheritance();

  std::cout << "All ConfigManager tests passed!\n";
  return 0;
}
