#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

extern "C" {
#include <stdlib.h> // setenv
}

#include "common/client/client_manager.h"
#include "common/config/core_config.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

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

// Handlers run on the session pump thread, so tests poll shared state instead of
// assuming synchronous delivery.
bool WaitFor(const std::function<bool()>& pred) {
  for (int i = 0; i < 100; ++i) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

struct CollectedLines {
  size_t Count() {
    const std::scoped_lock lock(mutex);
    return lines.size();
  }

  std::string At(size_t index) {
    const std::scoped_lock lock(mutex);
    return index < lines.size() ? lines[index] : "";
  }

  std::mutex mutex;
  std::vector<std::string> lines;
};

class TestEnvFixture {
public:
  TestEnvFixture() {
    base_dir_ = std::filesystem::temp_directory_path() / "vinput_test_client_mgr";
    std::filesystem::remove_all(base_dir_);
    std::filesystem::create_directories(base_dir_ / "config" / "vinput");
    std::filesystem::create_directories(base_dir_ / "runtime" / "vinput" / "clients");

    setenv("XDG_CONFIG_HOME", (base_dir_ / "config").c_str(), 1);
    setenv("XDG_RUNTIME_DIR", (base_dir_ / "runtime").c_str(), 1);
  }

  TestEnvFixture(const TestEnvFixture&) = delete;
  TestEnvFixture& operator=(const TestEnvFixture&) = delete;
  TestEnvFixture(TestEnvFixture&&) = delete;
  TestEnvFixture& operator=(TestEnvFixture&&) = delete;

  ~TestEnvFixture() {
    vinput::client::Shutdown();
    std::filesystem::remove_all(base_dir_);
  }

  static void WriteConfig(const std::string& json_content) {
    const auto config_path = vinput::path::CoreConfigPath();
    std::filesystem::create_directories(config_path.parent_path());
    ExpectTrue(vinput::file::AtomicWriteTextFile(config_path, json_content).has_value(),
               "WriteConfig failed");
  }

private:
  std::filesystem::path base_dir_;
};

void TestTriggerStartStopWithStdoutFramingAndStderrSink() {
  const TestEnvFixture fixture;

  TestEnvFixture::WriteConfig(R"({
    "version": ">=2.4.0",
    "triggers": [
      {
        "id": "trigger.mock.button",
        "command": "sh",
        "args": ["-c", "printf '{\"title\":\"TriggerReady\",\"body\":\"listening\"}\\n' >&2; printf '{\"type\":\"start\"}\n{\"type\":\"stop\"}\n'; sleep 30"],
        "auto_start": false
      }
    ]
  })");

  std::vector<vinput::notify::Notification> notifications;
  vinput::notify::NotificationBridge bridge(
      [&](const vinput::notify::Notification& n) { notifications.push_back(n); });

  CollectedLines commands;
  vinput::client::CommandHandler handler = [&](std::string_view line) {
    const std::scoped_lock lock(commands.mutex);
    commands.lines.emplace_back(line);
  };

  std::string error;
  ExpectTrue(vinput::client::Start("trigger.mock.button", &bridge, handler, &error),
             "Start should succeed: " + error);
  ExpectTrue(vinput::client::IsRunning("trigger.mock.button"),
             "trigger.mock.button should be reported as running");

  const pid_t pid = vinput::client::GetPid("trigger.mock.button");
  ExpectTrue(pid > 0, "trigger.mock.button pid should be positive");
  ExpectTrue(std::filesystem::exists(vinput::client::PidPath("trigger.mock.button")),
             "PID file should exist on disk");

  // Both JSONL command lines must survive chunk boundaries intact.
  ExpectTrue(WaitFor([&] { return commands.Count() >= 2; }),
             "Two framed stdout lines should arrive");
  ExpectEq(commands.At(0), "{\"type\":\"start\"}", "First command line mismatch");
  ExpectEq(commands.At(1), "{\"type\":\"stop\"}", "Second command line mismatch");

  // Notification from stderr should have arrived at the bridge.
  ExpectTrue(WaitFor([&] { return !notifications.empty(); }),
             "Notification should have been emitted by the trigger");
  ExpectEq(notifications.front().title, "TriggerReady", "Notification title mismatch");
  ExpectEq(notifications.front().body, "listening", "Notification body mismatch");

  // Duplicate start should fail.
  std::string duplicate_error;
  ExpectTrue(!vinput::client::Start("trigger.mock.button", &bridge, handler, &duplicate_error),
             "Duplicate start should be rejected");
  ExpectTrue(duplicate_error.find("already running") != std::string::npos,
             "Error should mention already running: " + duplicate_error);

  // Stop the trigger.
  std::string stop_error;
  ExpectTrue(vinput::client::Stop("trigger.mock.button", &stop_error, &bridge),
             "Stop should succeed: " + stop_error);
  ExpectTrue(!vinput::client::IsRunning("trigger.mock.button"),
             "trigger.mock.button should no longer be running");
  ExpectTrue(!std::filesystem::exists(vinput::client::PidPath("trigger.mock.button")),
             "PID file should be deleted after stop");
}

void TestReceiverLookupAndBroadcastEcho() {
  const TestEnvFixture fixture;

  TestEnvFixture::WriteConfig(R"({
    "version": ">=2.4.0",
    "receivers": [
      {
        "id": "receiver.mock.echo",
        "command": "cat",
        "args": [],
        "auto_start": false
      }
    ]
  })");

  CollectedLines echoed;
  vinput::client::CommandHandler handler = [&](std::string_view line) {
    const std::scoped_lock lock(echoed.mutex);
    echoed.lines.emplace_back(line);
  };

  std::string error;
  ExpectTrue(vinput::client::Start("receiver.mock.echo", nullptr, handler, &error),
             "Receiver under the receivers domain should start: " + error);
  ExpectTrue(vinput::client::IsRunning("receiver.mock.echo"),
             "receiver.mock.echo should be reported as running");

  // cat echoes stdin back to stdout, proving the broadcast path end to end.
  vinput::client::Broadcast("{\"type\":\"status\",\"status\":\"idle\"}");
  ExpectTrue(WaitFor([&] { return echoed.Count() >= 1; }),
             "Broadcast line should be echoed back by cat");
  ExpectEq(echoed.At(0), "{\"type\":\"status\",\"status\":\"idle\"}", "Echoed line mismatch");

  std::string stop_error;
  ExpectTrue(vinput::client::Stop("receiver.mock.echo", &stop_error),
             "Stop should succeed: " + stop_error);
}

void TestClientImmediateExitDetected() {
  const TestEnvFixture fixture;

  TestEnvFixture::WriteConfig(R"({
    "version": ">=2.4.0",
    "triggers": [
      {
        "id": "trigger.mock.failing",
        "command": "sh",
        "args": ["-c", "printf 'crash report\\n' >&2; exit 42"],
        "auto_start": false
      }
    ]
  })");

  std::vector<vinput::notify::Notification> notifications;
  vinput::notify::NotificationBridge bridge(
      [&](const vinput::notify::Notification& n) { notifications.push_back(n); });

  std::string error;
  ExpectTrue(!vinput::client::Start("trigger.mock.failing", &bridge, nullptr, &error),
             "Start should fail when child exits immediately");
  ExpectTrue(error.find("client exited immediately (code 42)") != std::string::npos,
             "Error should describe immediate exit code: " + error);
  ExpectTrue(!vinput::client::IsRunning("trigger.mock.failing"),
             "Failing client should not be tracked as running");
  ExpectTrue(!std::filesystem::exists(vinput::client::PidPath("trigger.mock.failing")),
             "PID file should not exist for a failing client");
}

void TestClientNotFound() {
  const TestEnvFixture fixture;

  TestEnvFixture::WriteConfig(R"({"version": ">=2.4.0"})");

  std::string error;
  ExpectTrue(!vinput::client::Start("trigger.missing", nullptr, nullptr, &error),
             "Starting an unknown client should fail");
  ExpectEq(error, "client not found", "Error message mismatch");
}

} // namespace

int main() {
  TestTriggerStartStopWithStdoutFramingAndStderrSink();
  TestReceiverLookupAndBroadcastEcho();
  TestClientImmediateExitDetected();
  TestClientNotFound();
  std::cout << "All client_manager tests passed!\n";
  return 0;
}
