#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/utils/process_utils.h"

namespace {

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

} // namespace

int main() {
  std::cout << "Running test_process_utils...\n";
  TestNormalExecution();
  TestCommandTimeout();
  TestChildExitWithLeakedPipeDoesNotHang();
  TestClosedFdsDoesNotBusySpin();
  std::cout << "All test_process_utils passed!\n";
  return 0;
}
