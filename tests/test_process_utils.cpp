#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "common/utils/process_utils.h"

namespace {

constexpr int kWaitNoHang = 1;

void ExpectTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ExpectTrue failed: " << message << "\n";
    std::abort();
  }
}

void TestNormalExecution() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "echo hello_process_utils"};
  spec.timeout_ms = 2000;

  const auto result = vinput::process::RunCommandWithInput(spec, {});
  ExpectTrue(!result.launch_failed, "Launch should succeed");
  ExpectTrue(!result.timed_out, "Command should not time out");
  ExpectTrue(result.exit_code == 0, "Exit code should be 0");
  ExpectTrue(result.stdout_text.find("hello_process_utils") != std::string::npos,
             "stdout should contain output");
}

void TestCommandTimeout() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "sleep 5"};
  spec.timeout_ms = 100;

  const auto start = std::chrono::steady_clock::now();
  const auto result = vinput::process::RunCommandWithInput(spec, {});
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  ExpectTrue(!result.launch_failed, "Launch should succeed");
  ExpectTrue(result.timed_out, "Command should time out");
  ExpectTrue(elapsed < 1000,
             "Timed-out command should abort quickly (elapsed=" + std::to_string(elapsed) + "ms)");
}

/**
 * Regression test for Issue #270:
 * Child process exits immediately, but a background grandchild / background process
 * holds the stdout/stderr pipe open. RunCommandWithInput must not hang or spin indefinitely.
 */
void TestChildExitWithLeakedPipeDoesNotHang() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  // Grandchild background sleep holds write end of stdout/stderr, parent exits immediately with 0.
  spec.args = {"-c", "sh -c 'sleep 5 >/dev/null 2>&1' & exit 0"};
  spec.timeout_ms = 1000;

  const auto start = std::chrono::steady_clock::now();
  const auto result = vinput::process::RunCommandWithInput(spec, {});
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  ExpectTrue(!result.launch_failed, "Launch should succeed");
  ExpectTrue(result.exit_code == 0, "Parent exit code should be 0");
  ExpectTrue(elapsed < 1500,
             "Exited child with open background pipes must finish bounded (elapsed=" +
                 std::to_string(elapsed) + "ms)");
}

/**
 * Regression test for Issue #270:
 * Command closes all fds (stdin, stdout, stderr) immediately and sleeps.
 * RunCommandWithInput must not busy-spin at 100% CPU when nfds == 0.
 */
void TestClosedFdsDoesNotBusySpin() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "exec 0<&- 1>&- 2>&-; sleep 0.15"};
  spec.timeout_ms = 1000;

  const auto start = std::chrono::steady_clock::now();
  const auto result = vinput::process::RunCommandWithInput(spec, {});
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  ExpectTrue(!result.launch_failed, "Launch should succeed");
  ExpectTrue(result.exit_code == 0, "Exit code should be 0");
  ExpectTrue(elapsed >= 100 && elapsed < 800,
             "Closed fd command should finish cleanly after child exits (elapsed=" +
                 std::to_string(elapsed) + "ms)");
}

/**
 * Regression test for Issue #270:
 * CreateCloexecPipe must set FD_CLOEXEC on both pipe ends, preventing descriptors
 * from being inherited by child processes across exec.
 */
void TestPipeCloexecBehavior() {
  int pipefd[2] = {-1, -1};
  ExpectTrue(vinput::process::CreateCloexecPipe(pipefd), "CreateCloexecPipe should succeed");
  ExpectTrue(pipefd[0] >= 0 && pipefd[1] >= 0, "Pipes should have valid descriptors");

  const int flags0 = fcntl(pipefd[0], F_GETFD);
  const int flags1 = fcntl(pipefd[1], F_GETFD);
  ExpectTrue(flags0 >= 0 && (flags0 & FD_CLOEXEC) != 0, "Pipe read end must have FD_CLOEXEC set");
  ExpectTrue(flags1 >= 0 && (flags1 & FD_CLOEXEC) != 0, "Pipe write end must have FD_CLOEXEC set");

  // Verify that spawning a child command does not inherit these descriptors.
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "test -e /proc/self/fd/" + std::to_string(pipefd[0]) +
                         " || test -e /proc/self/fd/" + std::to_string(pipefd[1])};
  spec.timeout_ms = 1000;
  const auto result = vinput::process::RunCommandWithInput(spec, {});
  ExpectTrue(!result.launch_failed, "Launch should succeed");
  // The child process does not have these fds, so test returns 1 (false).
  ExpectTrue(
      result.exit_code == 1,
      "Cloexec pipe descriptors must not be inherited by child process (expected exit code 1)");

  close(pipefd[0]);
  close(pipefd[1]);
}

void TestSpawnForMonitoring() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "echo test_var=$VINPUT_TEST_ENV >&2; exit 0"};
  spec.env["VINPUT_TEST_ENV"] = "safe_spawn_ok";

  vinput::process::SpawnedProcess process;
  std::string error;
  const bool spawned = vinput::process::SpawnForMonitoring(spec, "", &process, &error);
  ExpectTrue(spawned, "SpawnForMonitoring should succeed: " + error);
  ExpectTrue(process.pid > 0, "Spawned process PID should be > 0");
  ExpectTrue(process.stderr_fd >= 0, "Spawned process stderr_fd should be >= 0");

  std::string stderr_output;
  std::array<char, 128> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const ssize_t bytes_read = read(process.stderr_fd, buffer.data(), buffer.size());
    if (bytes_read > 0) {
      stderr_output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
    } else if (bytes_read == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  close(process.stderr_fd);
  int status = -1;
  bool exited = false;
  const auto exit_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < exit_deadline) {
    const auto wait_rc = waitpid(process.pid, &status, kWaitNoHang);
    if (wait_rc == process.pid) {
      exited = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ExpectTrue(exited, "Child process should exit within deadline");
  ExpectTrue(stderr_output.find("test_var=safe_spawn_ok") != std::string::npos,
             "Child process should receive environment variable via BuildEnvp (got: " +
                 stderr_output + ")");
  ExpectTrue(status == 0, "Child process should exit cleanly with status 0");
}

void TestRunCommandSilent() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "exit 0"};
  spec.timeout_ms = 1000;

  int rc = -1;
  ExpectTrue(vinput::process::RunCommandSilent(spec, &rc), "Silent command should succeed");
  ExpectTrue(rc == 0, "Exit code should be 0");

  spec.args = {"-c", "exit 42"};
  rc = -1;
  ExpectTrue(!vinput::process::RunCommandSilent(spec, &rc), "Non-zero exit should return false");
  ExpectTrue(rc == 42, "Exit code should be 42");

  spec.command = "";
  ExpectTrue(!vinput::process::RunCommandSilent(spec, &rc), "Empty command should fail");

  spec.command = "sh";
  spec.args = {"-c", "sleep 5"};
  spec.timeout_ms = 50;
  ExpectTrue(!vinput::process::RunCommandSilent(spec, &rc), "Timed out command should fail");
}

void TestRunCommandCaptureStdout() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "echo 'Volume: 0.75'"};
  spec.timeout_ms = 1000;

  std::string stdout_text;
  int rc = -1;
  ExpectTrue(vinput::process::RunCommandCaptureStdout(spec, &stdout_text, &rc),
             "Capture stdout should succeed");
  ExpectTrue(rc == 0, "Exit code should be 0");
  ExpectTrue(stdout_text == "Volume: 0.75\n", "Captured stdout mismatch: " + stdout_text);

  spec.args = {"-c", "echo 'error message' >&2; exit 2"};
  stdout_text.clear();
  rc = -1;
  ExpectTrue(!vinput::process::RunCommandCaptureStdout(spec, &stdout_text, &rc),
             "Failure command should return false");
  ExpectTrue(rc == 2, "Exit code should be 2");
  ExpectTrue(stdout_text.empty(), "Stdout should be empty");

  spec.command = "sh";
  spec.args = {"-c", "sleep 5"};
  spec.timeout_ms = 50;
  ExpectTrue(!vinput::process::RunCommandCaptureStdout(spec, &stdout_text, &rc),
             "Timed out command should return false");
}

} // namespace

int main() {
  std::cout << "Running test_process_utils...\n";
  TestPipeCloexecBehavior();
  TestNormalExecution();
  TestCommandTimeout();
  TestChildExitWithLeakedPipeDoesNotHang();
  TestClosedFdsDoesNotBusySpin();
  TestSpawnForMonitoring();
  TestRunCommandSilent();
  TestRunCommandCaptureStdout();
  std::cout << "All test_process_utils passed!\n";
  return 0;
}
