#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <stdlib.h> // setenv
}

#include "common/client/client_manager.h"
#include "common/config/core_config_types.h"
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

// The pump thread emits notifications while the test thread asserts on them, so
// every access funnels through this mutex-protected collector.
struct CollectedNotifications {
  void Add(const vinput::notify::Notification& n) {
    const std::scoped_lock lock(mutex);
    items.push_back(n);
  }

  size_t Count() {
    const std::scoped_lock lock(mutex);
    return items.size();
  }

  vinput::notify::Notification Front() {
    const std::scoped_lock lock(mutex);
    return items.empty() ? vinput::notify::Notification{} : items.front();
  }

  std::mutex mutex;
  std::vector<vinput::notify::Notification> items;
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

  CollectedNotifications notifications;
  vinput::notify::NotificationBridge bridge(
      [&](const vinput::notify::Notification& n) { notifications.Add(n); });

  CollectedLines commands;
  std::string captured_client_id;
  const vinput::client::CommandHandler handler = [&](std::string_view client_id,
                                                     std::string_view line) {
    const std::scoped_lock lock(commands.mutex);
    captured_client_id = std::string(client_id);
    commands.lines.emplace_back(line);
  };

  std::string error;
  ExpectTrue(vinput::client::Start("trigger.mock.button", &bridge, handler, &error),
             "Start should succeed: " + error);
  ExpectTrue(vinput::client::IsRunning("trigger.mock.button"),
             "trigger.mock.button should be reported as running");

  const auto pid = vinput::client::GetPid("trigger.mock.button");
  ExpectTrue(pid > 0, "trigger.mock.button pid should be positive");
  ExpectTrue(std::filesystem::exists(vinput::client::PidPath("trigger.mock.button")),
             "PID file should exist on disk");

  // Both JSONL command lines must survive chunk boundaries intact.
  ExpectTrue(WaitFor([&] { return commands.Count() >= 2; }),
             "Two framed stdout lines should arrive");
  ExpectEq(captured_client_id, "trigger.mock.button", "CommandHandler client_id mismatch");
  ExpectEq(commands.At(0), R"({"type":"start"})", "First command line mismatch");
  ExpectEq(commands.At(1), R"({"type":"stop"})", "Second command line mismatch");

  // Notification from stderr should have arrived at the bridge.
  ExpectTrue(WaitFor([&] { return notifications.Count() >= 1; }),
             "Notification should have been emitted by the trigger");
  ExpectEq(notifications.Front().title, "TriggerReady", "Notification title mismatch");
  ExpectEq(notifications.Front().body, "listening", "Notification body mismatch");

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
  const vinput::client::CommandHandler handler = [&](std::string_view /*client_id*/,
                                                     std::string_view line) {
    const std::scoped_lock lock(echoed.mutex);
    echoed.lines.emplace_back(line);
  };

  std::string error;
  ExpectTrue(vinput::client::Start("receiver.mock.echo", nullptr, handler, &error),
             "Receiver under the receivers domain should start: " + error);
  ExpectTrue(vinput::client::IsRunning("receiver.mock.echo"),
             "receiver.mock.echo should be reported as running");

  // Targeted SendTo nonexistent client must return false
  ExpectTrue(!vinput::client::SendTo("nonexistent.client", "test"),
             "SendTo on nonexistent client must return false");

  // Targeted SendTo to running client
  ExpectTrue(vinput::client::SendTo("receiver.mock.echo", R"({"type":"partial","text":"hello"})"),
             "SendTo on running client must succeed");
  ExpectTrue(WaitFor([&] { return echoed.Count() >= 1; }),
             "Targeted SendTo line should be echoed back by cat");
  ExpectEq(echoed.At(0), R"({"type":"partial","text":"hello"})", "Echoed line mismatch");

  // cat echoes stdin back to stdout, proving the broadcast path end to end.
  vinput::client::Broadcast(R"({"type":"status","status":"idle"})");
  ExpectTrue(WaitFor([&] { return echoed.Count() >= 2; }),
             "Broadcast line should be echoed back by cat");
  ExpectEq(echoed.At(1), R"({"type":"status","status":"idle"})", "Echoed line mismatch");

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

  CollectedNotifications notifications;
  vinput::notify::NotificationBridge bridge(
      [&](const vinput::notify::Notification& n) { notifications.Add(n); });

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

void TestAutoStartUsesInMemorySpec() {
  const TestEnvFixture fixture;

  // The disk config deliberately lacks this client: AutoStart must follow the
  // in-memory spec it was handed, not a stale re-read from disk.
  TestEnvFixture::WriteConfig(R"({"version": ">=2.4.0"})");

  CoreConfig config;
  ClientSpec spec;
  spec.id = "trigger.mock.autostart";
  spec.command = "sh";
  spec.args = {"-c", "sleep 30"};
  spec.autoStart = true;
  config.triggers.push_back(std::move(spec));

  vinput::client::AutoStart(config, nullptr, nullptr);
  ExpectTrue(vinput::client::IsRunning("trigger.mock.autostart"),
             "AutoStart should start the in-memory client spec");

  std::string stop_error;
  ExpectTrue(vinput::client::Stop("trigger.mock.autostart", &stop_error),
             "Stop should succeed: " + stop_error);
}

void TestGetClientStatuses() {
  const TestEnvFixture fixture;

  CoreConfig config;
  ClientSpec trigger;
  trigger.id = "trigger.status.test";
  trigger.command = "sh";
  trigger.args = {"-c", "sleep 30"};
  trigger.autoStart = true;
  config.triggers.push_back(std::move(trigger));

  ClientSpec receiver;
  receiver.id = "receiver.status.test";
  receiver.command = "sh";
  receiver.args = {"-c", "sleep 30"};
  receiver.autoStart = false;
  config.receivers.push_back(std::move(receiver));

  auto statuses = vinput::client::GetClientStatuses(config);
  ExpectEq(std::to_string(statuses.size()), "2", "Statuses count before start");
  ExpectEq(statuses[0].id, "trigger.status.test", "Trigger status ID");
  ExpectEq(statuses[1].id, "receiver.status.test", "Receiver status ID");
  ExpectTrue(!statuses[0].running && statuses[0].pid == -1, "Trigger should not be running");
  ExpectTrue(!statuses[1].running && statuses[1].pid == -1, "Receiver should not be running");

  vinput::client::AutoStart(config, nullptr, nullptr);
  statuses = vinput::client::GetClientStatuses(config);
  ExpectTrue(statuses[0].running && statuses[0].pid > 0,
             "Trigger should be running with positive pid");
  ExpectTrue(!statuses[1].running && statuses[1].pid == -1, "Receiver should still be stopped");

  std::string stop_error;
  ExpectTrue(vinput::client::Stop("trigger.status.test", &stop_error),
             "Stop should succeed: " + stop_error);
  statuses = vinput::client::GetClientStatuses(config);
  ExpectTrue(!statuses[0].running && statuses[0].pid == -1, "Trigger should be stopped after Stop");

  const auto self_pid = getpid();
  std::string write_err;
  ExpectTrue(vinput::client::WritePidFile("receiver.status.test", self_pid, &write_err),
             "WritePidFile should succeed: " + write_err);
  statuses = vinput::client::GetClientStatuses(config);
  ExpectTrue(statuses[1].running && statuses[1].pid == static_cast<std::int64_t>(self_pid),
             "Receiver should report running via on-disk PID record");
  vinput::client::RemovePidFile("receiver.status.test");
}

} // namespace

int main() {
  TestTriggerStartStopWithStdoutFramingAndStderrSink();
  TestReceiverLookupAndBroadcastEcho();
  TestClientImmediateExitDetected();
  TestClientNotFound();
  TestAutoStartUsesInMemorySpec();
  TestGetClientStatuses();
  std::cout << "All client_manager tests passed!\n";
  return 0;
}
