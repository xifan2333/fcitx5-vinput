#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
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

// Receives child output incrementally. Chunk boundaries are arbitrary, so a sink must not assume
// that a chunk equals a line or a message.
class PumpSink {
public:
  PumpSink() = default;
  PumpSink(const PumpSink&) = delete;
  PumpSink& operator=(const PumpSink&) = delete;
  PumpSink(PumpSink&&) = delete;
  PumpSink& operator=(PumpSink&&) = delete;
  virtual ~PumpSink() = default;

  virtual void OnStdout(std::string_view chunk) = 0;
  virtual void OnStderr(std::string_view chunk) = 0;
};

// Raw descriptors of one spawned child. The caller keeps ownership of every descriptor and of the
// child itself, so a pump never spawns, signals, reaps or closes anything on its own.
struct PumpChannels {
  int input_fd = -1;  // write end of the child's stdin, optional
  int stdout_fd = -1; // read end of the child's stdout, optional
  int stderr_fd = -1; // read end of the child's stderr, optional
};

// Reports whether the child already exited, filling `exit_code` with the normalized return code.
using ChildExitProbe = std::function<bool(int* exit_code)>;

struct PumpChild {
  PumpChannels channels;
  // Empty when the caller cannot probe the child; the pump then ends once all channels close.
  ChildExitProbe exited;
  // Invoked once after the last input byte was accepted, so the caller can close its own handle.
  std::function<void()> close_input;
};

struct PumpOptions {
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
  std::chrono::milliseconds poll_slice{50};
  std::chrono::milliseconds post_exit_grace{200};
  std::size_t read_budget_bytes = std::size_t{64} * 1024;
  // One-shot callers close the child's stdin after the input is drained; streaming callers keep it
  // open because they push another message later.
  bool close_input_after_write = true;
};

struct PumpResult {
  bool timed_out = false;
  // Set when the input was not fully accepted, either because the deadline expired or because the
  // child stopped reading.
  bool input_incomplete = false;
  bool child_exited = false;
  int child_exit_code = -1;
  bool stdout_closed = false;
  bool stderr_closed = false;
};

// Drives one spawned child until its channels close and it exits, or until the deadline expires.
// This is the single implementation of deadline-bounded, non-blocking, bidirectional draining that
// every child-process call site shares.
PumpResult PumpIo(const PumpChild& child, std::span<const std::byte> input,
                  const PumpOptions& options, PumpSink& sink);

// One-shot synchronous run with a deadline: buffered output, no incremental callbacks.
CommandResult RunOnce(const CommandSpec& spec, std::span<const std::byte> input = {});

// Compatibility alias kept so call sites that predate RunOnce stay readable.
inline CommandResult RunCommandWithInput(const CommandSpec& spec,
                                         std::span<const std::byte> input) {
  return RunOnce(spec, input);
}
bool RunCommandSilent(const CommandSpec& spec, int* exit_code = nullptr);
Expected<std::string> RunCommandCaptureStdout(const CommandSpec& spec);
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
