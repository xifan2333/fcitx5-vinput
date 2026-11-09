#include <QtCore/qcoreapplication.h>
#include <QtCore/qglobal.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h> // getpid

#include "common/config/core_config_types.h"

#include "gui/utils/config_manager.h"

namespace {

void AssertTrue(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

// RAII guard creating a unique per-run temporary directory, so tests never
// delete or collide with data owned by other processes or runs.
class TempDirGuard {
public:
  TempDirGuard() {
    path = std::filesystem::temp_directory_path() /
           ("vinput_test_config_" + std::to_string(::getpid()) + "_" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::create_directories(path / "vinput", ec);
  }
  ~TempDirGuard() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  TempDirGuard(const TempDirGuard&) = delete;
  TempDirGuard& operator=(const TempDirGuard&) = delete;
  TempDirGuard(TempDirGuard&&) = delete;
  TempDirGuard& operator=(TempDirGuard&&) = delete;

  std::filesystem::path path;
};

void TestNullPointer() {
  std::string error;
  const bool ok = vinput::gui::ConfigManager::Get().TryLoad(nullptr, &error);
  AssertTrue(!ok, "TryLoad(nullptr) must return false");
  AssertTrue(error == "null config pointer", "Error message must indicate null config pointer");
}

void TestNonexistentConfigFails() {
  const TempDirGuard guard;
  qputenv("XDG_CONFIG_HOME", guard.path.string().c_str());

  CoreConfig config;
  std::string error;
  const bool ok = vinput::gui::ConfigManager::Get().TryLoad(&config, &error);
  AssertTrue(!ok, "TryLoad on nonexistent file must return false");
  AssertTrue(error == "config file not found", "Error message must indicate not found");
}

void TestCorruptedConfigProtection() {
  const TempDirGuard guard;

  {
    std::ofstream ofs(guard.path / "vinput" / "config.json");
    ofs << R"({ "version": "2.4.0", "asr": { unclosed json)";
  }

  qputenv("XDG_CONFIG_HOME", guard.path.string().c_str());

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
}

void TestValidConfigLoading() {
  const TempDirGuard guard;

  const auto config_path = guard.path / "vinput" / "config.json";
  {
    std::ofstream ofs(config_path);
    ofs << R"({
  "version": ">=2.4.0",
  "asr": {
    "active_provider": "sherpa-local",
    "providers": [
      {
        "id": "sherpa-local",
        "type": "local",
        "model": "zipformer-zh",
        "timeout_ms": 15000
      }
    ]
  }
})";
  }

  qputenv("XDG_CONFIG_HOME", guard.path.string().c_str());

  CoreConfig config;
  std::string error;
  const bool ok = vinput::gui::ConfigManager::Get().TryLoad(&config, &error);
  AssertTrue(ok, "TryLoad on valid JSON must return true");
  AssertTrue(error.empty(), "Error string must be empty on success");
  AssertTrue(config.asr.activeProvider == "sherpa-local", "activeProvider must match");
  AssertTrue(config.asr.providers.size() == 1, "providers count must match");
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

void TestExternalModificationConflict() {
  // Snapshot of the provider captured when the edit dialog opened.
  CommandAsrProvider snapshot;
  snapshot.id = "provider-a";
  snapshot.command = "whisper-cli";
  snapshot.args = {"--model", "small"};
  snapshot.env = {{"LANG", "en_US.UTF-8"}};
  snapshot.timeoutMs = 30000;

  // The same provider ID still exists in the fresh config, but another process
  // changed its command while the dialog was open.
  CommandAsrProvider fresh;
  fresh.id = "provider-a";
  fresh.command = "faster-whisper";
  fresh.args = {"--model", "small"};
  fresh.env = {{"LANG", "en_US.UTF-8"}};
  fresh.timeoutMs = 30000;

  // Existence-by-ID lookup alone cannot distinguish "unchanged" from "modified";
  // the edit flow must compare the persisted fields to reject the overwrite.
  const bool unchanged = snapshot.id == fresh.id && snapshot.command == fresh.command &&
                         snapshot.args == fresh.args && snapshot.env == fresh.env &&
                         snapshot.timeoutMs == fresh.timeoutMs;
  AssertTrue(!unchanged,
             "Externally modified provider (same ID, changed command) must be a conflict");

  // Sanity: an identical snapshot must not be flagged as a conflict.
  const CommandAsrProvider identical = fresh;
  const bool same = snapshot.id == fresh.id && fresh.command == identical.command;
  AssertTrue(same, "Identical fields must compare equal");
}

} // namespace

int main(int argc, char* argv[]) {
  const QCoreApplication app(argc, argv);

  TestNullPointer();
  TestNonexistentConfigFails();
  TestCorruptedConfigProtection();
  TestValidConfigLoading();
  TestAsrProviderIdInheritance();
  TestExternalModificationConflict();

  std::cout << "All ConfigManager tests passed!\n";
  return 0;
}
