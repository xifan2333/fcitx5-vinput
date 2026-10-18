#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/config/core_config_types.h"

#include "daemon/asr/backends/command_streaming_backend.h"
#include "daemon/asr/runtime/recognition_contract.h"

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

  // The normal completion path lasts ~450 ms; a final result uses 200 ms grace.
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
  const bool ok = session->PushAudio(std::span<const int16_t>{}, &error);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ExpectTrue(!ok && !error.empty(), "a filled provider pipe must eventually report failure");
  ExpectTrue(elapsed < std::chrono::seconds(2), "outbound write must respect its timeout");
  session->Cancel();
  alarm(0);
  AssertChildAlreadyReaped(child_pid);
}

/** Cancel of a never-started session must not spawn a new child. */
void TestCancelDoesNotStartProvider() {
  const std::string marker = GetUniquePidFilePath(8);
  unlink(marker.c_str());
  CommandAsrProvider provider;
  provider.id = "test-never-started-cancel";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo started > " + marker + "; exec sleep 30"};
  provider.timeoutMs = 250;
  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed");
  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed");
  session->Cancel();
  ExpectTrue(!session->PushAudio(std::span<const int16_t>{}, &error),
             "PushAudio after Cancel must fail");
  ExpectTrue(!session->Finish(&error), "Finish after Cancel must fail");
  const bool started = access(marker.c_str(), F_OK) == 0;
  unlink(marker.c_str());
  ExpectTrue(!started, "Cancel must not spawn a provider solely to stop it");
}

} // namespace

int main() {
  std::cout << "Running command streaming process lifecycle tests...\n";
  TestSilentSessionReapsChild();
  TestDestructionReapsChild();
  TestFinalTextSessionReapsChild();
  TestCancelReapsChild();
  TestOutputEofWait(false, 5);
  TestOutputEofWait(true, 6);
  TestStalledProviderWriteIsBounded();
  TestCancelDoesNotStartProvider();
  std::cout << "All command streaming process lifecycle tests passed successfully!\n";
  return 0;
}
