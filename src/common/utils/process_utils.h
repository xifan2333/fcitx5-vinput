#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <type_traits>
#include <vector>

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

CommandResult RunCommandWithInput(const CommandSpec& spec, std::span<const std::byte> input);
bool RunCommandSilent(const CommandSpec& spec, int* exit_code = nullptr);
bool RunCommandCaptureStdout(const CommandSpec& spec, std::string* stdout_text,
                             int* exit_code = nullptr);
bool SpawnForMonitoring(const CommandSpec& spec, const std::filesystem::path& working_dir,
                        SpawnedProcess* process_out, std::string* error);
bool CreateCloexecPipe(int pipefd[2]);

// Drains complete newline-terminated lines out of an incrementally filled
// process output buffer, leaving any unterminated tail in place. A trailing
// '\r' is stripped so CRLF streams behave like LF ones. When `flush_tail` is
// set, the remaining (unterminated) tail is delivered as a final line and the
// buffer is cleared. A callback returning `false` stops draining immediately.
template <typename Callback>
void DrainLinesFromBuffer(std::string& buffer, Callback on_line, bool flush_tail = false) {
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

    if constexpr (std::is_void_v<std::invoke_result_t<Callback, std::string_view>>) {
      on_line(line);
    } else {
      if (!static_cast<bool>(on_line(line))) {
        should_stop = true;
        break;
      }
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
    if constexpr (std::is_void_v<std::invoke_result_t<Callback, std::string_view>>) {
      on_line(tail);
    } else {
      (void)on_line(tail);
    }
    buffer.clear();
  }
}

} // namespace vinput::process
