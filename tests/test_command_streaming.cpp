#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "daemon/asr/backends/command_streaming_backend.h"
#include "daemon/asr/runtime/recognition_contract.h"

namespace {

void ExpectTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ExpectTrue failed: " << message << "\n";
    std::abort();
  }
}

std::string GetUniquePidFilePath(int id) {
  return "/tmp/vinput_test_streaming_pid_" + std::to_string(getpid()) + "_" + std::to_string(id) +
         ".tmp";
}

pid_t ReadPidFileWithRetry(const std::string& path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream file(path);
    if (file.is_open()) {
      pid_t pid = -1;
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

void AssertChildAlreadyReaped(pid_t pid) {
  int status = 0;
  const pid_t rc = waitpid(pid, &status, WNOHANG);
  // If the child was already properly reaped by CommandStreamingSession,
  // waitpid returns -1 with ECHILD.
  ExpectTrue(rc == -1 && errno == ECHILD,
             "Child pid " + std::to_string(pid) + " should have already been reaped (rc=" +
                 std::to_string(rc) + ", errno=" + std::to_string(errno) + ")");
}

// Regression test for Issue #264:
// When a session finishes without any final text (e.g. silent recording),
// the provider exits, and the child process MUST be reaped without becoming a zombie.
void TestSilentSessionReapsChild() {
  const std::string pid_file = GetUniquePidFilePath(1);
  unlink(pid_file.c_str());

  CommandAsrProvider provider;
  provider.id = "test-silent-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file +
                             " && echo '{\"type\": \"session_started\"}'"
                             " && echo '{\"type\": \"closed\"}'"
                             " && exit 0"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  // Push empty audio to start child process
  ExpectTrue(session->PushAudio({}, &error), "PushAudio failed: " + error);

  const pid_t child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file for silent session");

  // Finish session with no text
  session->Finish(&error);
  auto events = session->PollEvents();
  ExpectTrue(!events.empty(), "Events should not be empty");

  // Verify child was reaped and is not a zombie
  AssertChildAlreadyReaped(child_pid);
}

// Test that destroying a session without calling Finish terminates and reaps the child process.
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

    ExpectTrue(session->PushAudio({}, &error), "PushAudio failed: " + error);

    const pid_t child_pid = ReadPidFileWithRetry(pid_file);
    ExpectTrue(child_pid > 0, "Failed to read child pid file for destruct session");

    // Session destroyed at end of scope without Finish()
    session.reset();

    // Verify child was terminated and reaped by destructor
    AssertChildAlreadyReaped(child_pid);
  }
}

// Test normal session with final text
void TestFinalTextSessionReapsChild() {
  const std::string pid_file = GetUniquePidFilePath(3);
  unlink(pid_file.c_str());

  CommandAsrProvider provider;
  provider.id = "test-final-text-provider";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo $$ > " + pid_file +
                             " && echo '{\"type\": \"session_started\"}'"
                             " && echo '{\"type\": \"final\", \"text\": \"hello world\"}'"
                             " && echo '{\"type\": \"closed\"}'"
                             " && exit 0"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandStreamingBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandStreamingBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  ExpectTrue(session->PushAudio({}, &error), "PushAudio failed: " + error);

  const pid_t child_pid = ReadPidFileWithRetry(pid_file);
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

// Test Cancel reaps child
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

  ExpectTrue(session->PushAudio({}, &error), "PushAudio failed: " + error);

  const pid_t child_pid = ReadPidFileWithRetry(pid_file);
  ExpectTrue(child_pid > 0, "Failed to read child pid file for cancel session");

  session->Cancel();

  AssertChildAlreadyReaped(child_pid);
}

} // namespace

int main() {
  std::cout << "Running command streaming process lifecycle tests...\n";
  TestSilentSessionReapsChild();
  TestDestructionReapsChild();
  TestFinalTextSessionReapsChild();
  TestCancelReapsChild();
  std::cout << "All command streaming process lifecycle tests passed successfully!\n";
  return 0;
}
