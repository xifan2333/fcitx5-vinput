#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

extern "C" {
#include <time.h> // pid_t: <chrono> pulls <time.h> in first, so <sys/types.h> would be a no-op
}

#include "common/utils/expected.h"

namespace vinput::process {

struct CommandSpec {
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  int timeout_ms = 0;
};

struct CommandResult {
  int exit_code = -1;
  bool timed_out = false;
  bool launch_failed = false;
  std::string stdout_text;
  std::string stderr_text;
};

struct SpawnedProcess {
  pid_t pid = -1;
  int stderr_fd = -1;
};

// Runs `spec` to completion, feeding `stdin_bytes` to its standard input, and returns its
// normalized result: exit code, timeout flag and the full stdout/stderr text. The child runs in its
// own session and `spec.timeout_ms` bounds the whole call; on expiry it is killed.
//
// This is the module's whole one-shot interface. Descriptor plumbing (polling, non-blocking mode,
// read budgets, exit probing, reaping) stays behind the seam so callers only think about the
// provider command they want to run.
CommandResult Run(const CommandSpec& spec, std::span<const std::byte> stdin_bytes = {});

Expected<SpawnedProcess> SpawnForMonitoring(const CommandSpec& spec,
                                            const std::filesystem::path& working_dir);
bool CreateCloexecPipe(int pipefd[2]);

// Forcefully terminates `pid` with SIGKILL and reaps it, tolerating children that
// already exited or were reaped elsewhere. Returns true when the child is known
// to be gone.
bool KillAndReap(pid_t pid);

// Non-blocking wait for child process exit. Returns true if the child has exited,
// populating `exit_code` with its normalized return code if non-null.
bool TryReapProcess(pid_t pid, int* exit_code = nullptr);

// Requests graceful termination, waits for a bounded interval, then escalates
// to a forced kill and performs a final bounded reap.
void TerminateProcessBounded(pid_t pid, std::chrono::milliseconds graceful_timeout,
                             std::chrono::milliseconds force_timeout);

// Reads the process start time (field 22 of /proc/<pid>/stat, in clock ticks
// since boot). Together with the pid this identifies one specific instance:
// once a process exits the kernel may hand its pid to an unrelated one, so a
// bare pid is not a stable handle. Returns 0 when the process is gone or the
// field cannot be parsed.
[[nodiscard]] std::uint64_t ProcessStartTime(pid_t pid);

// True when the live process at `pid` is still the instance that was recorded
// with `start_time`. A record without a start time cannot be re-identified, so
// it never matches anything.
[[nodiscard]] bool ProcessIdentityMatches(pid_t pid, std::uint64_t start_time);

// How a termination attempt ended.
enum class TerminateOutcome : std::uint8_t {
  Exited,       // The recorded instance is gone.
  NotFound,     // Nothing runs at `pid`, or the pid now belongs to another instance.
  StillRunning, // Observed alive after the forced kill.
  Unconfirmed,  // The exit could not be observed either way.
  Failed,       // A signal could not be delivered; `error` explains why.
};

struct TerminateResult {
  TerminateOutcome outcome = TerminateOutcome::Failed;
  // The signal whose delivery failed, which tells a rejected graceful request
  // apart from a failed forced kill: an unreachable process versus one that
  // refuses to die. Zero unless `outcome` is Failed.
  int failed_signal = 0;
  std::string error;
};

// Stops the process instance identified by (`pid`, `start_time`): SIGTERM, a
// bounded wait for `graceful_timeout`, then SIGKILL and a final bounded wait of
// `force_timeout`.
//
// Every signal is delivered through a pidfd when the kernel offers one, so the
// pinned instance can never be confused with a recycled pid; without a pidfd the
// recorded identity is re-checked immediately before each signal instead. This is
// the seam for stopping something this process did not spawn - a child of ours
// cannot have its pid recycled before it is reaped, so plain pid signalling is
// enough there.
TerminateResult TerminateGracefully(pid_t pid, std::uint64_t start_time,
                                    std::chrono::milliseconds graceful_timeout,
                                    std::chrono::milliseconds force_timeout);

// Drains complete newline-terminated lines out of an incrementally filled
// process output buffer, leaving any unterminated tail in place. A trailing
// '\r' is stripped so CRLF streams behave like LF ones. When `flush_tail` is
// set, the remaining (unterminated) tail is delivered as a final line and the
// buffer is cleared. A callback returning `false` stops draining immediately.
template <typename Callback>
void DrainLinesFromBuffer(std::string& buffer, Callback on_line, bool flush_tail = false) {
  // 归一化回调结果：返回 void 的回调视为“继续”，返回 bool 的回调按其值决定
  const auto deliver = [&on_line](std::string_view line) -> bool {
    if constexpr (std::is_void_v<std::invoke_result_t<Callback, std::string_view>>) {
      on_line(line);
      return true;
    } else {
      return static_cast<bool>(on_line(line));
    }
  };

  size_t processed = 0;
  size_t start = 0;
  bool should_stop = false;

  while (start < buffer.size()) {
    const size_t end = buffer.find('\n', start);
    if (end == std::string::npos) {
      break;
    }
    size_t len = end - start;
    if (len > 0 && buffer[start + len - 1] == '\r') {
      --len;
    }
    const std::string_view line(buffer.data() + start, len);
    start = end + 1;
    processed = start;

    if (!deliver(line)) {
      should_stop = true;
      break;
    }
  }

  if (processed > 0) {
    buffer.erase(0, processed);
  }

  if (flush_tail && !should_stop && !buffer.empty()) {
    std::string_view tail(buffer);
    if (tail.back() == '\r') {
      tail.remove_suffix(1);
    }
    (void)deliver(tail);
    buffer.clear();
  }
}

} // namespace vinput::process
