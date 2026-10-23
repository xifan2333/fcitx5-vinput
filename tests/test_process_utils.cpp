#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <span>
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

  const auto result = vinput::process::Run(spec);
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
  const auto result = vinput::process::Run(spec);
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
 * holds the stdout/stderr pipe open. Run must not hang or spin indefinitely.
 */
void TestChildExitWithLeakedPipeDoesNotHang() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  // Grandchild background sleep holds write end of stdout/stderr, parent exits immediately with 0.
  spec.args = {"-c", "sh -c 'sleep 5 >/dev/null 2>&1' & exit 0"};
  spec.timeout_ms = 1000;

  const auto start = std::chrono::steady_clock::now();
  const auto result = vinput::process::Run(spec);
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
 * Run must not busy-spin at 100% CPU when nfds == 0.
 */
void TestClosedFdsDoesNotBusySpin() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "exec 0<&- 1>&- 2>&-; sleep 0.15"};
  spec.timeout_ms = 1000;

  const auto start = std::chrono::steady_clock::now();
  const auto result = vinput::process::Run(spec);
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
  const auto result = vinput::process::Run(spec);
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

  auto process_res = vinput::process::SpawnForMonitoring(spec, "");
  ExpectTrue(process_res.has_value(),
             "SpawnForMonitoring should succeed: " + (process_res ? "" : process_res.error()));
  const auto process = *process_res;
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

/**
 * Run must deliver stdin and then close it, which is what makes a filtering child exit.
 */
void TestRunFeedsStdin() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "cat"};
  spec.timeout_ms = 2000;

  const std::string payload = "dictation text\n";
  const auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));
  const auto result = vinput::process::Run(spec, bytes);

  ExpectTrue(!result.launch_failed, "Launch should succeed");
  ExpectTrue(!result.timed_out, "Feeding stdin should not time out");
  ExpectTrue(result.exit_code == 0, "Child should exit cleanly after stdin closes");
  ExpectTrue(result.stdout_text == payload, "Child should echo the input: " + result.stdout_text);
}

/**
 * A child that writes more than one read budget must not lose or truncate output.
 */
void TestRunDrainsLargeOutput() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "seq 1 20000"};
  spec.timeout_ms = 5000;

  const auto result = vinput::process::Run(spec);

  ExpectTrue(!result.launch_failed, "Launch should succeed");
  ExpectTrue(!result.timed_out, "Large output must not time out");
  ExpectTrue(result.exit_code == 0, "Exit code should be 0");

  const auto line_count = static_cast<std::size_t>(
      std::count(result.stdout_text.begin(), result.stdout_text.end(), '\n'));
  ExpectTrue(line_count == 20000, "All lines must be captured, got " + std::to_string(line_count));
}

/**
 * Both streams must be captured completely when both flood at once. Each writer exceeds a pipe
 * buffer, so the child can only exit if neither channel is held back behind the other one; a
 * starved channel would block the child in write() and turn this run into a timeout with truncated
 * output.
 */
void TestRunCapturesBothStreamsWhenBothFlood() {
  constexpr std::size_t kLineCount = 20000;

  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "seq 1 20000 & seq 1 20000 >&2; wait"};
  spec.timeout_ms = 5000;

  const auto result = vinput::process::Run(spec);

  ExpectTrue(!result.launch_failed, "Launch should succeed");
  ExpectTrue(!result.timed_out, "Both streams must be drained without reaching the deadline");
  ExpectTrue(result.exit_code == 0, "Exit code should be 0");

  const auto out_lines = static_cast<std::size_t>(
      std::count(result.stdout_text.begin(), result.stdout_text.end(), '\n'));
  const auto err_lines = static_cast<std::size_t>(
      std::count(result.stderr_text.begin(), result.stderr_text.end(), '\n'));
  ExpectTrue(out_lines == kLineCount, "stdout must be complete, got " + std::to_string(out_lines));
  ExpectTrue(err_lines == kLineCount, "stderr must be complete, got " + std::to_string(err_lines));
}

/**
 * A child that closes every standard descriptor and keeps running must still be bounded by the
 * deadline: closing the channels is not an exit, so Run may not perform an unbounded wait.
 */
void TestRunTimesOutWhenChildClosesEveryDescriptor() {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "exec 0<&- 1>&- 2>&-; sleep 5"};
  spec.timeout_ms = 200;

  const auto start = std::chrono::steady_clock::now();
  const auto result = vinput::process::Run(spec);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  ExpectTrue(!result.launch_failed, "Launch should succeed");
  ExpectTrue(result.timed_out, "Closed channels must not disable the deadline");
  ExpectTrue(elapsed < 1500, "Deadline must still stop a running child (elapsed=" +
                                 std::to_string(elapsed) + "ms)");
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
  TestRunFeedsStdin();
  TestRunDrainsLargeOutput();
  TestRunCapturesBothStreamsWhenBothFlood();
  TestRunTimesOutWhenChildClosesEveryDescriptor();
  std::cout << "All test_process_utils passed!\n";
  return 0;
}
