#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "common/config/core_config.h"
#include "common/config/core_config_store.h"
#include "common/config/core_config_types.h"
#include "common/llm/adapter_manager.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/process_utils.h"

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

class TestEnvFixture {
public:
  TestEnvFixture() {
    base_dir_ = std::filesystem::temp_directory_path() / "vinput_test_adapter_mgr";
    std::filesystem::remove_all(base_dir_);
    std::filesystem::create_directories(base_dir_ / "config" / "vinput");
    std::filesystem::create_directories(base_dir_ / "runtime" / "vinput" / "adapters");

    setenv("XDG_CONFIG_HOME", (base_dir_ / "config").c_str(), 1);
    setenv("XDG_RUNTIME_DIR", (base_dir_ / "runtime").c_str(), 1);
  }

  ~TestEnvFixture() {
    vinput::adapter::Shutdown();
    std::filesystem::remove_all(base_dir_);
  }

  void WriteConfig(const std::string& json_content) {
    const auto config_path = vinput::path::CoreConfigPath();
    std::filesystem::create_directories(config_path.parent_path());
    ExpectTrue(vinput::file::AtomicWriteTextFile(config_path, json_content).has_value(),
               "WriteConfig failed");
  }

private:
  std::filesystem::path base_dir_;
};

void TestAdapterNormalStartAndStopWithStderrSink() {
  TestEnvFixture fixture;

  fixture.WriteConfig(R"({
    "version": ">=2.4.0",
    "llm": {
      "adapters": [
        {
          "id": "mock-adapter",
          "command": "sh",
          "args": ["-c", "printf '{\"title\":\"AdapterReady\",\"body\":\"listening on 8990\"}\\n' >&2; sleep 30"],
          "auto_start": false
        }
      ]
    }
  })");

  std::vector<vinput::notify::Notification> notifications;
  vinput::notify::NotificationBridge bridge(
      [&](const vinput::notify::Notification& n) { notifications.push_back(n); });

  std::string error;
  ExpectTrue(vinput::adapter::Start("mock-adapter", &bridge, &error),
             "Start should succeed: " + error);
  ExpectTrue(vinput::adapter::IsRunning("mock-adapter"),
             "mock-adapter should be reported as running");

  const pid_t pid = vinput::adapter::GetPid("mock-adapter");
  ExpectTrue(pid > 0, "mock-adapter pid should be positive");
  ExpectTrue(std::filesystem::exists(vinput::adapter::PidPath("mock-adapter")),
             "PID file should exist on disk");

  // Notification from stderr should have arrived at the bridge
  ExpectTrue(!notifications.empty(), "Notification should have been emitted by the adapter");
  ExpectEq(notifications.front().title, "AdapterReady", "Notification title mismatch");
  ExpectEq(notifications.front().body, "listening on 8990", "Notification body mismatch");

  // Duplicate start should fail
  std::string duplicate_error;
  ExpectTrue(!vinput::adapter::Start("mock-adapter", &bridge, &duplicate_error),
             "Duplicate start should be rejected");
  ExpectTrue(duplicate_error.find("already running") != std::string::npos,
             "Error should mention already running: " + duplicate_error);

  // Stop the adapter
  std::string stop_error;
  ExpectTrue(vinput::adapter::Stop("mock-adapter", &stop_error, &bridge),
             "Stop should succeed: " + stop_error);
  ExpectTrue(!vinput::adapter::IsRunning("mock-adapter"),
             "mock-adapter should no longer be running");
  ExpectTrue(!std::filesystem::exists(vinput::adapter::PidPath("mock-adapter")),
             "PID file should be deleted after stop");
}

void TestAdapterImmediateExitDetected() {
  TestEnvFixture fixture;

  fixture.WriteConfig(R"({
    "version": ">=2.4.0",
    "llm": {
      "adapters": [
        {
          "id": "failing-adapter",
          "command": "sh",
          "args": ["-c", "printf 'crash report\\n' >&2; exit 42"],
          "auto_start": false
        }
      ]
    }
  })");

  std::vector<vinput::notify::Notification> notifications;
  vinput::notify::NotificationBridge bridge(
      [&](const vinput::notify::Notification& n) { notifications.push_back(n); });

  std::string error;
  ExpectTrue(!vinput::adapter::Start("failing-adapter", &bridge, &error),
             "Start should fail when child exits immediately");
  ExpectTrue(error.find("adapter exited immediately (code 42)") != std::string::npos,
             "Error should describe immediate exit code: " + error);
  ExpectTrue(!vinput::adapter::IsRunning("failing-adapter"),
             "Failing adapter should not be tracked as running");
  ExpectTrue(!std::filesystem::exists(vinput::adapter::PidPath("failing-adapter")),
             "PID file should not remain after immediate failure");

  // Notification from the crash stderr should still reach the bridge
  ExpectTrue(!notifications.empty(), "Crash stderr should be delivered to NotificationBridge");
  ExpectEq(notifications.front().body, "crash report", "Crash body mismatch");
}

void TestAdapterAutoStartAndShutdown() {
  TestEnvFixture fixture;

  fixture.WriteConfig(R"({
    "version": ">=2.4.0",
    "llm": {
      "adapters": [
        {
          "id": "auto-adapter-1",
          "command": "sh",
          "args": ["-c", "sleep 30"],
          "auto_start": true
        },
        {
          "id": "manual-adapter-2",
          "command": "sh",
          "args": ["-c", "sleep 30"],
          "auto_start": false
        }
      ]
    }
  })");

  vinput::notify::NotificationBridge bridge([](const auto&) {});

  auto config = LoadCoreConfig();
  NormalizeCoreConfig(&config);
  vinput::adapter::AutoStart(config, &bridge);

  ExpectTrue(vinput::adapter::IsRunning("auto-adapter-1"), "auto-adapter-1 should be running");
  ExpectTrue(!vinput::adapter::IsRunning("manual-adapter-2"),
             "manual-adapter-2 should not be running");

  vinput::adapter::Shutdown();
  ExpectTrue(!vinput::adapter::IsRunning("auto-adapter-1"),
             "auto-adapter-1 should be stopped after Shutdown()");
}

} // namespace

int main() {
  std::cout << "Running test_adapter_manager...\n";
  TestAdapterNormalStartAndStopWithStderrSink();
  TestAdapterImmediateExitDetected();
  TestAdapterAutoStartAndShutdown();
  std::cout << "All test_adapter_manager passed!\n";
  return 0;
}
