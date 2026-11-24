#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/notify/notification_bridge.h"

#include "daemon/asr/backends/command_streaming_backend.h"
#include "daemon/asr/runtime/recognition_contract.h"

#include "scoped_xdg_config.h"

namespace {

/** Abort test execution if condition is not satisfied. */
void ExpectTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ExpectTrue failed: " << message << "\n";
    std::abort();
  }
}

/** Generate a unique temporary path for storing the spawned child pid. */
std::string GetUniquePidFilePath(int id) {
  return "/tmp/vinput_test_streaming_pid_" + std::to_string(getpid()) + "_" + std::to_string(id) +
         ".tmp";
}

/** Poll for the written child pid file until available or timeout. */
int ReadPidFileWithRetry(const std::string& path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream file(path);
    if (file.is_open()) {
      int pid = -1;
      if (file >> pid && pid > 0) {
        file.close();
        unlink(path.c_str());
        return pid;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

/** Verify that the child process was reaped and is not a zombie. */
void AssertChildAlreadyReaped(int pid) {
  int status = 0;
  const auto rc = waitpid(pid, &status, 0);
  // If the child was already properly reaped by CommandStreamingSession,
  // waitpid returns -1 with ECHILD.
  ExpectTrue(rc == -1 && errno == ECHILD,
             "Child pid " + std::to_string(pid) + " should have already been reaped (rc=" +
                 std::to_string(rc) + ", errno=" + std::to_string(errno) + ")");
}

/**
 * Regression test for Issue #264:
 * When a session finishes without any final text (e.g. silent recording),
 * the provider exits, and the child process MUST be reaped without becoming a zombie.
 */
void TestSilentSessionReapsChild() {
  const std::string pid_file = GetUniquePidFilePath(1);
  unlink(pid_file.c_str());

  CommandAsrProvider provider;
  provider.id = "test-silent-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c",
                   "echo $$ > " + pid_file +
                       " && echo '{\"type\": \"session_started\"}'"
                       " && while read -r line; do case \"$line\" in *finish*) break ;; esac; done"
                       " && echo '{\"type\": \"closed\"}'"
                       " && exit 0"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  // Push empty audio to start child process
  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed: " + error);

  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file for silent session");

  // Finish session with no text; returns false because no text was produced
  const bool finish_ok = session->Finish(&error);
  ExpectTrue(!finish_ok, "Finish should return false for silent session with no text");
  ExpectTrue(!error.empty(), "Error message should not be empty");

  auto events = session->PollEvents();
  ExpectTrue(!events.empty(), "Events should not be empty");
  bool saw_error_event = false;
  bool saw_completed_event = false;
  for (const auto& ev : events) {
    if (ev.kind == vinput::daemon::asr::RecognitionEventKind::Error) {
      saw_error_event = true;
    }
    if (ev.kind == vinput::daemon::asr::RecognitionEventKind::Completed) {
      saw_completed_event = true;
    }
  }
  ExpectTrue(saw_error_event, "Expected Error event in silent session");
  ExpectTrue(saw_completed_event, "Expected Completed event in silent session");

  // Verify child was reaped and is not a zombie
  AssertChildAlreadyReaped(child_pid);
}

/** Test that destroying a session without calling Finish terminates and reaps the child process. */
void TestDestructionReapsChild() {
  const std::string pid_file = GetUniquePidFilePath(2);
  unlink(pid_file.c_str());

  CommandAsrProvider provider;
  provider.id = "test-destruct-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file +
                             " && echo '{\"type\": \"session_started\"}'"
                             " && sleep 30"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  {
    auto session = backend->CreateSession(&error);
    ExpectTrue(session != nullptr, "CreateSession failed: " + error);

    ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error),
               "PushAudio failed: " + error);

    const int child_pid = ReadPidFileWithRetry(pid_file);
    ExpectTrue(child_pid > 0, "Failed to read child pid file for destruct session");

    // Session destroyed at end of scope without Finish()
    session.reset();

    // Verify child was terminated and reaped by destructor
    AssertChildAlreadyReaped(child_pid);
  }
}

/** Test normal session with final text reaps child. */
void TestFinalTextSessionReapsChild() {
  const std::string pid_file = GetUniquePidFilePath(3);
  unlink(pid_file.c_str());

  CommandAsrProvider provider;
  provider.id = "test-final-text-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c",
                   "echo $$ > " + pid_file +
                       " && echo '{\"type\": \"session_started\"}'"
                       " && while read -r line; do case \"$line\" in *finish*) break ;; esac; done"
                       " && echo '{\"type\": \"final\", \"text\": \"hello world\"}'"
                       " && echo '{\"type\": \"closed\"}'"
                       " && exit 0"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed: " + error);

  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file for final text session");

  ExpectTrue(session->Finish(&error), "Finish failed: " + error);

  const auto events = session->PollEvents();
  bool saw_final = false;
  for (const auto& ev : events) {
    if (ev.kind == vinput::daemon::asr::RecognitionEventKind::FinalText &&
        ev.text == "hello world") {
      saw_final = true;
    }
  }
  ExpectTrue(saw_final, "Did not receive expected final text");

  AssertChildAlreadyReaped(child_pid);
}

/** Test Cancel terminates and reaps child. */
void TestCancelReapsChild() {
  const std::string pid_file = GetUniquePidFilePath(4);
  unlink(pid_file.c_str());

  CommandAsrProvider provider;
  provider.id = "test-cancel-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file +
                             " && echo '{\"type\": \"session_started\"}'"
                             " && sleep 30"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed: " + error);

  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file for cancel session");

  session->Cancel();
  ExpectTrue(!session->Finish(&error), "Finish after Cancel must fail");
  ExpectTrue(!error.empty(), "Finish after Cancel reports non-empty error");

  AssertChildAlreadyReaped(child_pid);
}

/** A provider may close both output pipes before exiting; wait without a CPU spin. */
void TestOutputEofWait(bool produce_final, int id) {
  const std::string pid_file = GetUniquePidFilePath(id);
  unlink(pid_file.c_str());

  std::string script =
      "echo $$ > " + pid_file +
      " && echo '{\"type\":\"session_started\"}'"
      " && while IFS= read -r line; do case \"$line\" in *finish*) break ;; esac; done";
  if (produce_final) {
    script += R"( && echo '{"type":"final","text":"hello eof"}')";
  }
  // exec preserves the child PID while closing both pipes and waiting.
  script += " && exec 1>&- 2>&- && exec sleep 0.45";

  CommandAsrProvider provider;
  provider.id = produce_final ? "test-final-before-eof" : "test-no-final-before-eof";
  provider.command = "/bin/sh";
  provider.args = {"-c", script};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);
  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);
  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed: " + error);
  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid before EOF test");

  const std::clock_t cpu_start = std::clock();
  const auto wall_start = std::chrono::steady_clock::now();
  const bool finished = session->Finish(&error);
  const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - wall_start)
                           .count();
  const double cpu_ms = static_cast<double>(std::clock() - cpu_start) * 1000.0 / CLOCKS_PER_SEC;

  // The normal completion path waits until the child exits (~450 ms).
  // Avoid asserting exact timing on a shared CI runner.
  ExpectTrue(wall_ms >= (produce_final ? 100 : 300), "EOF fixture should exercise wait state");
  ExpectTrue(wall_ms < 3000, "EOF wait should preserve bounded completion");
  ExpectTrue(cpu_ms < 120.0, "EOF wait should not burn an entire CPU core");
  ExpectTrue(finished == produce_final, "Final-result success semantics must be unchanged");

  const auto events = session->PollEvents();
  int final_events = 0;
  int error_events = 0;
  int completed_events = 0;
  for (const auto& event : events) {
    if (event.kind == vinput::daemon::asr::RecognitionEventKind::FinalText) {
      ++final_events;
      ExpectTrue(event.text == "hello eof", "Unexpected final text");
    }
    if (event.kind == vinput::daemon::asr::RecognitionEventKind::Error) {
      ++error_events;
    }
    if (event.kind == vinput::daemon::asr::RecognitionEventKind::Completed) {
      ++completed_events;
    }
  }
  ExpectTrue(final_events == (produce_final ? 1 : 0), "Final event count must be preserved");
  ExpectTrue(error_events == (produce_final ? 0 : 1), "Error event count must be preserved");
  ExpectTrue(completed_events == 1, "Completed must be emitted exactly once");
  AssertChildAlreadyReaped(child_pid);
  std::cout << "EOF wait: wall=" << wall_ms << "ms CPU=" << cpu_ms << "ms\n";
}

/** A non-reading Provider must not trap PushAudio or Cancel indefinitely. */
void TestStalledProviderWriteIsBounded() {
  const std::string pid_file = GetUniquePidFilePath(7);
  unlink(pid_file.c_str());
  CommandAsrProvider provider;
  provider.id = "test-stalled-write-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file + " && exec sleep 30"};
  provider.timeoutMs = 250;
  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);
  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);
  std::vector<int16_t> long_chunk(100000, 512);
  ExpectTrue(session->PushAudio(long_chunk, &error), "first PushAudio should queue samples");
  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "stalled provider did not start");
  const auto started = std::chrono::steady_clock::now();
  alarm(6);
  bool ok = true;
  for (int i = 0; i < 1000; ++i) {
    ok = session->PushAudio(long_chunk, &error);
    if (!ok) {
      break;
    }
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ExpectTrue(!ok && !error.empty(), "a filled provider pipe must eventually report failure");
  ExpectTrue(elapsed < std::chrono::seconds(2), "outbound write must respect its timeout");
  session->Cancel();
  alarm(0);
  AssertChildAlreadyReaped(child_pid);
}

/** Eager start contract (Issue #709): CreateSession must start the provider immediately,
 *  and Cancel must terminate + reap it without spawning a second child. */
void TestEagerStartThenCancelReapsChild() {
  const std::string pid_file = GetUniquePidFilePath(8);
  unlink(pid_file.c_str());
  CommandAsrProvider provider;
  provider.id = "test-eager-start-cancel";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file + "; exec sleep 30"};
  provider.timeoutMs = 250;
  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed");
  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed");

  // CreateSession must already have spawned the provider, before any audio arrives.
  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "CreateSession must eagerly start the provider");

  session->Cancel();
  ExpectTrue(!session->PushAudio(std::span<const int16_t>{}, &error),
             "PushAudio after Cancel must fail");
  ExpectTrue(!session->Finish(&error), "Finish after Cancel must fail");
  AssertChildAlreadyReaped(child_pid);
}

/** Warmup contract (Issue #709): Warmup on command-streaming must be a zero-side-effect
 *  no-op that returns true and does not spawn a child process. */
void TestWarmupIsNoOpAndDoesNotSpawnChild() {
  const std::string pid_file = GetUniquePidFilePath(100);
  unlink(pid_file.c_str());
  CommandAsrProvider provider;
  provider.id = "test-warmup-no-child";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file + "; exec sleep 30"};
  provider.timeoutMs = 250;
  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed");

  ExpectTrue(backend->Warmup(&error), "Warmup must succeed as a no-op");
  ExpectTrue(error.empty(), "Warmup must not report an error");

  const bool spawned = access(pid_file.c_str(), F_OK) == 0;
  unlink(pid_file.c_str());
  ExpectTrue(!spawned, "Warmup must not spawn a child process");
}

/** Test Interrupt() immediately breaks a blocked Finish() wait. */
void TestInterruptWakesBlockedFinish() {
  const std::string pid_file = GetUniquePidFilePath(9);
  unlink(pid_file.c_str());

  CommandAsrProvider provider;
  provider.id = "test-interrupt-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file +
                             " && echo '{\"type\": \"session_started\"}'"
                             " && exec sleep 30"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed: " + error);

  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file for interrupt test");

  std::thread interrupter([&session]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session->Interrupt();
  });

  const auto started = std::chrono::steady_clock::now();
  const bool finish_ok = session->Finish(&error);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  interrupter.join();

  ExpectTrue(!finish_ok, "Finish should return false after interrupt");
  ExpectTrue(elapsed < std::chrono::milliseconds(2500),
             "Interrupt() should wake Finish immediately without waiting for 10s timeout");

  AssertChildAlreadyReaped(child_pid);
}

/** Test that stderr chunks are fed to NotificationBridge and properly parsed and flushed. */
void TestStderrPipedToNotificationBridge() {
  const std::string pid_file = GetUniquePidFilePath(10);
  unlink(pid_file.c_str());

  std::vector<vinput::notify::Notification> received;
  vinput::notify::NotificationBridge bridge([&](const auto& n) { received.push_back(n); });

  CommandAsrProvider provider;
  provider.id = "test-bridge-provider";
  provider.command = "/bin/sh";
  provider.args = {
      "-c", "echo $$ > " + pid_file +
                " && echo '{\"type\": \"session_started\"}'"
                " && echo '{\"title\": \"Custom Warning\", \"body\": \"Rate limit reached\"}' >&2"
                " && echo 'Verbatim plain error' >&2"
                " && while read -r line; do case \"$line\" in *finish*) break ;; esac; done"
                " && echo '{\"type\": \"final\", \"text\": \"test output\"}'"
                " && echo '{\"type\": \"closed\"}'"
                " && exit 0"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error, &bridge);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed: " + error);

  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file for bridge test");

  ExpectTrue(session->Finish(&error), "Finish failed: " + error);

  AssertChildAlreadyReaped(child_pid);

  ExpectTrue(received.size() == 2,
             "Expected 2 notifications from stderr, got " + std::to_string(received.size()));
  ExpectTrue(received[0].title == "Custom Warning",
             "Expected custom title, got " + received[0].title);
  ExpectTrue(received[0].body == "Rate limit reached",
             "Expected json body, got " + received[0].body);
  ExpectTrue(received[1].title == "test-bridge-provider",
             "Expected provider title/id, got " + received[1].title);
  ExpectTrue(received[1].body == "Verbatim plain error",
             "Expected plain body, got " + received[1].body);
}

/** Test killed_by_us semantics: user cancellation does not emit Error, provider crash does. */
void TestKilledByUsVsAbnormalExit() {
  // Scenario A: User Cancel (killed_by_us == true)
  {
    const std::string pid_file = GetUniquePidFilePath(11);
    unlink(pid_file.c_str());

    CommandAsrProvider provider;
    provider.id = "test-killed-by-us-provider";
    provider.command = "/bin/sh";
    provider.args = {"-c", "echo $$ > " + pid_file +
                               " && echo '{\"type\": \"session_started\"}'"
                               " && exec sleep 30"};
    provider.timeoutMs = 5000;

    std::string error;
    auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
    ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed");

    auto session = backend->CreateSession(&error);
    ExpectTrue(session != nullptr, "CreateSession failed");
    ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed");

    const int child_pid = ReadPidFileWithRetry(pid_file);
    ExpectTrue(child_pid > 0, "Failed to read child pid file");

    session->Cancel();
    AssertChildAlreadyReaped(child_pid);

    const auto events = session->PollEvents();
    bool saw_error = false;
    for (const auto& ev : events) {
      if (ev.kind == vinput::daemon::asr::RecognitionEventKind::Error) {
        saw_error = true;
      }
    }
    ExpectTrue(!saw_error, "User cancellation must not emit Error event (killed_by_us = true)");
  }

  // Scenario B: Provider crash / abnormal exit (killed_by_us == false, exit_code != 0)
  {
    const std::string pid_file = GetUniquePidFilePath(12);
    unlink(pid_file.c_str());

    CommandAsrProvider provider;
    provider.id = "test-provider-crash";
    provider.command = "/bin/sh";
    provider.args = {"-c", "echo $$ > " + pid_file +
                               " && echo '{\"type\": \"session_started\"}'"
                               " && echo 'provider crashed internally' >&2"
                               " && exit 42"};
    provider.timeoutMs = 5000;

    std::string error;
    auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
    ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed");

    auto session = backend->CreateSession(&error);
    ExpectTrue(session != nullptr, "CreateSession failed");
    ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed");

    const int child_pid = ReadPidFileWithRetry(pid_file);
    ExpectTrue(child_pid > 0, "Failed to read child pid file");

    const bool finish_ok = session->Finish(&error);
    ExpectTrue(!finish_ok, "Finish must fail when provider exits with non-zero status");
    AssertChildAlreadyReaped(child_pid);

    const auto events = session->PollEvents();
    bool saw_error = false;
    for (const auto& ev : events) {
      if (ev.kind == vinput::daemon::asr::RecognitionEventKind::Error) {
        saw_error = true;
      }
    }
    ExpectTrue(saw_error, "Abnormal provider exit must emit Error event (killed_by_us = false)");
  }
}

/**
 * Test that PushAudio flushes audio frames immediately to child stdin
 * without artificial 100ms withholding or waiting for a subsequent chunk / Finish.
 */
void TestImmediateAudioEmission() {
  const std::string received_file =
      "/tmp/vinput_test_streaming_immediate_" + std::to_string(getpid()) + ".tmp";
  unlink(received_file.c_str());

  CommandAsrProvider provider;
  provider.id = "test-immediate-audio-provider";
  provider.command = "/bin/sh";
  provider.args = {
      "-c", "while IFS= read -r line; do "
            "  case \"$line\" in "
            "    *\"audio\"*) echo \"$line\" >> " +
                received_file +
                " ;; "
                "    *\"finish\"*) echo '{\"type\": \"final\", \"text\": \"ok\"}' ; break ;; "
                "  esac; "
                "done && echo '{\"type\": \"closed\"}' && exit 0"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  const std::array<int16_t, 160> samples = {100, -200, 300, -400};
  // Push a single chunk without finishing or pushing a second chunk
  ExpectTrue(session->PushAudio(samples, &error), "PushAudio failed: " + error);

  // Poll for the written received_file; with zero-latency push, it must appear immediately
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool saw_audio = false;
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream file(received_file);
    if (file.is_open()) {
      std::string line;
      if (std::getline(file, line) && line.find("audio") != std::string::npos) {
        saw_audio = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ExpectTrue(saw_audio, "Child process should receive audio chunk immediately without withholding");

  const bool finish_ok = session->Finish(&error);
  ExpectTrue(finish_ok, "Finish should succeed: " + error);

  const auto events = session->PollEvents();
  bool saw_final = false;
  for (const auto& ev : events) {
    if (ev.kind == vinput::daemon::asr::RecognitionEventKind::FinalText && ev.text == "ok") {
      saw_final = true;
    }
  }
  ExpectTrue(saw_final, "Session should receive final text");

  unlink(received_file.c_str());
}

/** Test that obsolete stdout {"type": "error"} is ignored and not queued as an Error event,
 * while stderr notifications continue to be routed cleanly to NotificationBridge. */
void TestStdoutErrorNotQueuedAsEvent() {
  const std::string pid_file = GetUniquePidFilePath(15);
  unlink(pid_file.c_str());

  std::vector<vinput::notify::Notification> received_notifications;
  vinput::notify::NotificationBridge bridge(
      [&](const auto& n) { received_notifications.push_back(n); });

  CommandAsrProvider provider;
  provider.id = "test-stdout-error-provider";
  provider.command = "/bin/sh";
  provider.args = {
      "-c", "echo $$ > " + pid_file +
                " && echo '{\"type\": \"session_started\"}'"
                " && echo '{\"type\": \"error\", \"message\": \"legacy stdout error\"}'"
                " && echo '{\"title\": \"Valid Stderr Notice\", \"body\": \"Channel check\"}' >&2"
                " && while read -r line; do case \"$line\" in *finish*) break ;; esac; done"
                " && echo '{\"type\": \"final\", \"text\": \"valid text\"}'"
                " && echo '{\"type\": \"closed\"}'"
                " && exit 0"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error, &bridge);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed: " + error);

  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file");

  ExpectTrue(session->Finish(&error), "Finish failed: " + error);
  AssertChildAlreadyReaped(child_pid);

  const auto events = session->PollEvents();
  bool saw_error = false;
  bool saw_final = false;
  for (const auto& ev : events) {
    if (ev.kind == vinput::daemon::asr::RecognitionEventKind::Error) {
      saw_error = true;
    }
    if (ev.kind == vinput::daemon::asr::RecognitionEventKind::FinalText &&
        ev.text == "valid text") {
      saw_final = true;
    }
  }
  ExpectTrue(!saw_error, "Legacy stdout error must not be queued as RecognitionEventKind::Error");
  ExpectTrue(saw_final, "FinalText event should be preserved");

  ExpectTrue(received_notifications.size() == 1,
             "NotificationBridge should only receive stderr notification, got " +
                 std::to_string(received_notifications.size()));
  ExpectTrue(received_notifications[0].title == "Valid Stderr Notice",
             "Expected custom stderr title, got " + received_notifications[0].title);
  ExpectTrue(received_notifications[0].body == "Channel check",
             "Expected stderr notification body, got " + received_notifications[0].body);
}

/**
 * Issue #598 regression test:
 * When a streaming provider child process crashes or exits prematurely,
 * subsequent PushAudio/SendJsonLine must report a precise error identifying the exit code.
 */
void TestPrematureExitErrorReporting() {
  CommandAsrProvider provider;
  provider.id = "test-premature-exit";
  provider.command = "/bin/sh";
  provider.args = {"-c", "exit 42"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  // Push minimal audio to ensure provider child process is started
  std::vector<int16_t> initial_chunk(160, 0);
  (void)session->PushAudio(initial_chunk, &error);

  // Await child process exit
  for (int i = 0; i < 50; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)session->PollEvents();
  }

  std::vector<int16_t> chunk(800, 100);
  const bool ok = session->PushAudio(chunk, &error);
  ExpectTrue(!ok, "PushAudio must fail when provider exited prematurely");
  ExpectTrue(error.find("exited prematurely with code 42") != std::string::npos,
             "error message should report precise premature exit code 42: " + error);
}

void TestEarlyClosedReportsFailure() {
  CommandAsrProvider provider;
  provider.id = "test-early-closed-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c", R"(cat > /dev/null && echo '{"type": "closed"}' && exit 0)"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  // PushAudio drains the early "closed" event
  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed");

  // First Finish with nullptr must detect failure without dropping error text
  ExpectTrue(!session->Finish(nullptr),
             "Finish(nullptr) must report failure when closed early without text");

  // Subsequent Finish calls must preserve the failure result and provide error text
  std::string second_error;
  ExpectTrue(!session->Finish(&second_error), "Subsequent Finish must continue to fail");
  ExpectTrue(second_error == "Streaming ASR provider returned no text.",
             "Subsequent Finish preserves exact error message even after Finish(nullptr)");
}

void TestStderrLocalizedTitleFromCache() {
  const vinput::test::ScopedXdgConfigDir xdg("vinput_test_stream_i18n");
  vinput::test::ScopedXdgConfigDir::WriteLocalI18n(
      R"({"test-stream-localized.title":"Localized Stream ASR"})");

  std::vector<vinput::notify::Notification> received;
  vinput::notify::NotificationBridge bridge([&](const auto& n) { received.push_back(n); });

  CommandAsrProvider provider;
  provider.id = "test-stream-localized";
  provider.command = "/bin/sh";
  provider.args = {"-c",
                   "echo '{\"type\": \"session_started\"}'"
                   " && echo 'plain stream notice' >&2"
                   " && while read -r line; do case \"$line\" in *finish*) break ;; esac; done"
                   " && echo '{\"type\": \"final\", \"text\": \"ok\"}'"
                   " && echo '{\"type\": \"closed\"}'"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error, &bridge);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed");
  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed");
  ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "PushAudio failed");
  ExpectTrue(session->Finish(&error), "Finish failed");
  ExpectTrue(received.size() == 1 && received[0].title == "Localized Stream ASR",
             "Streaming stderr notification must use localized provider title");
}

struct MidFinalSession {
  std::unique_ptr<vinput::daemon::asr::AsrBackend> backend;
  std::unique_ptr<vinput::daemon::asr::RecognitionSession> session;
  int child_pid = -1;
};

MidFinalSession StartAndAwaitMidFinal(int pid_id, const std::string& provider_id,
                                      const std::string& shell_script,
                                      const std::string& expected_mid_final) {
  const std::string pid_file = GetUniquePidFilePath(pid_id);
  unlink(pid_file.c_str());

  CommandAsrProvider provider;
  provider.id = provider_id;
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file + " && " + shell_script};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  std::vector<int16_t> chunk(800, 100);
  ExpectTrue(session->PushAudio(chunk, &error), "PushAudio failed: " + error);

  const int child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file");

  bool saw_mid_final = false;
  for (int i = 0; i < 50 && !saw_mid_final; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ExpectTrue(session->PushAudio(std::span<const int16_t>{}, &error), "Poll PushAudio failed");
    for (const auto& ev : session->PollEvents()) {
      if (ev.kind == vinput::daemon::asr::RecognitionEventKind::FinalText &&
          ev.text == expected_mid_final) {
        saw_mid_final = true;
      }
    }
  }
  ExpectTrue(saw_mid_final, "Expected mid-session final before Finish");
  return {.backend = std::move(backend), .session = std::move(session), .child_pid = child_pid};
}

/**
 * Issue #756 regression test:
 * Mid-session and post-finish final events must not cause Finish() to exit
 * early and kill the provider before {"type": "closed"} arrives.
 */
void TestMultipleFinalsAwaitClosedAfterFinish() {
  auto active = StartAndAwaitMidFinal(
      16, "test-multiple-finals-tail",
      "echo '{\"type\": \"session_started\"}'"
      " && while IFS= read -r line; do"
      "   case \"$line\" in"
      "     *audio*) echo '{\"type\": \"final\", \"text\": \"first segment\"}' ;;"
      "     *finish*) echo '{\"type\": \"final\", \"text\": \"first segment second\"}';"
      " sleep 0.35; echo '{\"type\": \"final\", \"text\": \"first segment second tail\"}';"
      " echo '{\"type\": \"closed\"}'; break ;;"
      "   esac;"
      " done",
      "first segment");

  std::string error;
  ExpectTrue(active.session->Finish(&error), "Finish failed: " + error);
  const auto events = active.session->PollEvents();
  std::string last_final;
  for (const auto& ev : events) {
    if (ev.kind == vinput::daemon::asr::RecognitionEventKind::FinalText) {
      last_final = ev.text;
    }
  }
  ExpectTrue(last_final == "first segment second tail",
             "Expected full trailing final text after finish, got: " + last_final);
  AssertChildAlreadyReaped(active.child_pid);
}

/**
 * Issue #756 regression test:
 * When a provider already emitted a mid-session final and emits only
 * {"type": "closed"} after finish (no new speech), Finish() must succeed
 * without reporting "returned no text".
 */
void TestMidSessionFinalFollowedByCleanCloseAfterFinish() {
  auto active = StartAndAwaitMidFinal(
      17, "test-mid-final-clean-close",
      "echo '{\"type\": \"session_started\"}'"
      " && while IFS= read -r line; do"
      "   case \"$line\" in"
      "     *audio*) echo '{\"type\": \"final\", \"text\": \"only segment\"}' ;;"
      "     *finish*) sleep 0.30; echo '{\"type\": \"closed\"}'; break ;;"
      "   esac;"
      " done",
      "only segment");

  std::string error;
  ExpectTrue(active.session->Finish(&error), "Finish after clean close must succeed: " + error);
  AssertChildAlreadyReaped(active.child_pid);
}

} // namespace

int main() {
  std::cout << "Running command streaming process lifecycle tests...\n";
  TestImmediateAudioEmission();
  TestStdoutErrorNotQueuedAsEvent();
  TestSilentSessionReapsChild();
  TestDestructionReapsChild();
  TestFinalTextSessionReapsChild();
  TestCancelReapsChild();
  TestOutputEofWait(false, 5);
  TestOutputEofWait(true, 6);
  TestStalledProviderWriteIsBounded();
  TestEagerStartThenCancelReapsChild();
  TestWarmupIsNoOpAndDoesNotSpawnChild();
  TestInterruptWakesBlockedFinish();
  TestStderrPipedToNotificationBridge();
  TestKilledByUsVsAbnormalExit();
  TestPrematureExitErrorReporting();
  TestEarlyClosedReportsFailure();
  TestStderrLocalizedTitleFromCache();
  TestMultipleFinalsAwaitClosedAfterFinish();
  TestMidSessionFinalFollowedByCleanCloseAfterFinish();
  std::cout << "All command streaming process lifecycle tests passed successfully!\n";
  return 0;
}
