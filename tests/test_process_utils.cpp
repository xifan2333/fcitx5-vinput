#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <span>
#include <string>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "common/utils/process_utils.h"

namespace {

constexpr int kWaitNoHang = 1;
constexpr auto kTestGracefulTimeout = std::chrono::milliseconds(300);
constexpr auto kTestForceTimeout = std::chrono::milliseconds(2000);

// Upper bound on the readiness handshake. It only guards against a miswritten test script: the
// scripts below announce readiness immediately.
constexpr int kReadyTimeoutMs = 5000;

// Spawns an unreaped child running |script| with /bin/sh, so termination can be exercised against a
// process the test itself owns. The child's stdout is a pipe into the parent, and the script has to
// write a single readiness byte into it once its signal handlers are installed: returning earlier
// would let a test signal a child that still runs the default SIGTERM action, which kills it
// outright instead of exercising the trap. A script that exits without writing releases the parent
// through EOF. When neither happens the handshake gives up, kills the child and reports failure, so
// a forgotten readiness byte can never hang the suite.
pid_t SpawnStandaloneChild(const char* script) {
  std::array<int, 2> ready{-1, -1};
  if (pipe(ready.data()) != 0) {
    return -1;
  }
  const pid_t pid = fork();
  if (pid == 0) {
    (void)dup2(ready.at(1), STDOUT_FILENO);
    (void)close(ready.at(0));
    (void)close(ready.at(1));
    execl("/bin/sh", "sh", "-c", script, static_cast<char*>(nullptr));
    _exit(127);
  }
  (void)close(ready.at(1));
  bool handshake_ok = false;
  if (pid > 0) {
    pollfd descriptor{};
    descriptor.fd = ready.at(0);
    descriptor.events = POLLIN;
    int rc = ::poll(&descriptor, 1, kReadyTimeoutMs);
    while (rc < 0 && errno == EINTR) {
      rc = ::poll(&descriptor, 1, kReadyTimeoutMs);
    }
    if (rc > 0) {
      char byte = 0;
      ssize_t n = read(ready.at(0), &byte, 1);
      while (n < 0 && errno == EINTR) {
        n = read(ready.at(0), &byte, 1);
      }
      // Zero bytes read is EOF, which is how a child that exits without writing announces itself.
      handshake_ok = n >= 0;
    }
  }
  (void)close(ready.at(0));
  if (pid > 0 && !handshake_ok) {
    (void)vinput::process::KillAndReap(pid);
    return -1;
  }
  return pid;
}

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

/**
 * TerminateGracefully must stop the exact instance recorded by (pid, start_time),
 * on the plain SIGTERM path, and report the exit.
 */
void TestTerminateGracefullyStopsOnSigterm() {
  const pid_t pid =
      SpawnStandaloneChild("trap 'exit 0' TERM; echo ready; while true; do sleep 0.05; done");
  ExpectTrue(pid > 0, "Standalone child should spawn");

  const std::uint64_t start_time = vinput::process::ProcessStartTime(pid);
  ExpectTrue(start_time != 0, "A live process must have a readable start time");

  const auto result = vinput::process::TerminateGracefully(pid, start_time, kTestGracefulTimeout,
                                                           kTestForceTimeout);
  ExpectTrue(result.outcome == vinput::process::TerminateOutcome::Exited,
             "A SIGTERM handler must end the process gracefully");
  ExpectTrue(result.error.empty(), "A graceful exit must not report an error: " + result.error);

  (void)vinput::process::KillAndReap(pid);
}

/**
 * A process that ignores SIGTERM must be escalated to SIGKILL, and only after the
 * graceful window has elapsed.
 */
void TestTerminateGracefullyEscalatesToSigkill() {
  const pid_t pid =
      SpawnStandaloneChild("trap '' TERM; echo ready; while true; do sleep 0.05; done");
  ExpectTrue(pid > 0, "Standalone child should spawn");
  const std::uint64_t start_time = vinput::process::ProcessStartTime(pid);
  ExpectTrue(start_time != 0, "A live process must have a readable start time");

  const auto start = std::chrono::steady_clock::now();
  const auto result = vinput::process::TerminateGracefully(pid, start_time, kTestGracefulTimeout,
                                                           kTestForceTimeout);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  ExpectTrue(result.outcome == vinput::process::TerminateOutcome::Exited,
             "SIGKILL escalation must end a SIGTERM-ignoring process");
  ExpectTrue(elapsed >= static_cast<long long>(kTestGracefulTimeout.count()),
             "The graceful window must be honoured before escalating (elapsed=" +
                 std::to_string(elapsed) + "ms)");

  (void)vinput::process::KillAndReap(pid);
}

/**
 * The pid-reuse guard: a record whose start time does not match the live process
 * must never be signalled, because that process is somebody else.
 */
void TestTerminateGracefullyRejectsStaleIdentity() {
  const pid_t pid = SpawnStandaloneChild("echo ready; exec sleep 30");
  ExpectTrue(pid > 0, "Standalone child should spawn");

  const std::uint64_t start_time = vinput::process::ProcessStartTime(pid);
  ExpectTrue(start_time != 0, "A live process must have a readable start time");

  const auto result = vinput::process::TerminateGracefully(pid, start_time + 1,
                                                           kTestGracefulTimeout, kTestForceTimeout);
  ExpectTrue(result.outcome == vinput::process::TerminateOutcome::NotFound,
             "A mismatched identity must be reported as not found");
  ExpectTrue(!vinput::process::TryReapProcess(pid), "A mismatched identity must not be signalled");
  ExpectTrue(vinput::process::ProcessIdentityMatches(pid, start_time),
             "The untouched process must still be alive");

  (void)vinput::process::KillAndReap(pid);
}

/**
 * A record whose process is already gone must be reported as not found instead of
 * waiting out the full timeout.
 */
void TestTerminateGracefullyReportsMissingProcess() {
  const pid_t pid = SpawnStandaloneChild("exit 0");
  ExpectTrue(pid > 0, "Standalone child should spawn");
  const std::uint64_t start_time = vinput::process::ProcessStartTime(pid);
  ExpectTrue(start_time != 0, "A live process must have a readable start time");

  ExpectTrue(vinput::process::KillAndReap(pid), "The short-lived child must be reaped");

  const auto start = std::chrono::steady_clock::now();
  const auto result = vinput::process::TerminateGracefully(pid, start_time, kTestGracefulTimeout,
                                                           kTestForceTimeout);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  ExpectTrue(result.outcome == vinput::process::TerminateOutcome::NotFound,
             "A reaped process must be reported as not found");
  ExpectTrue(elapsed < static_cast<long long>(kTestGracefulTimeout.count()),
             "A gone process must not wait out the graceful window (elapsed=" +
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
  TestTerminateGracefullyStopsOnSigterm();
  TestTerminateGracefullyEscalatesToSigkill();
  TestTerminateGracefullyRejectsStaleIdentity();
  TestTerminateGracefullyReportsMissingProcess();
  std::cout << "All test_process_utils passed!\n";
  return 0;
}
