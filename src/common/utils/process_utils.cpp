#include "common/utils/process_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <subprocess.hpp>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <signal.h> // SIGKILL
#include <time.h>   // pid_t
}

#include "common/utils/expected.h"
#include "common/utils/interpolation.h"

namespace vinput::process {

namespace {

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::vector<std::string> BuildCmd(const CommandSpec& spec) {
  std::vector<std::string> cmd;
  cmd.reserve(spec.args.size() + 1);
  cmd.push_back(spec.command);
  for (const auto& arg : spec.args) {
    cmd.push_back(arg);
  }
  return cmd;
}

std::map<std::string, std::string> BuildEnvMap(const CommandSpec& spec) {
  std::map<std::string, std::string> env_map;
  for (char* const* env = environ; env != nullptr && *env != nullptr; ++env) {
    const std::string entry(*env);
    const auto eq_pos = entry.find('=');
    if (eq_pos != std::string::npos) {
      env_map[entry.substr(0, eq_pos)] = entry.substr(eq_pos + 1);
    }
  }

  for (const auto& [key, value] : spec.env) {
    env_map[key] = vinput::interpolate::InterpolateEnv(value);
  }
  return env_map;
}

struct DrainOutcome {
  std::size_t bytes_read = 0;
  bool closed = false;
};

// Reads at most `budget` bytes from `fd` and forwards every chunk to `emit`. The budget keeps one
// fast writer from starving the other channels of the same pump iteration.
struct DrainFdBudgeted {
  int fd = -1;
  std::size_t budget = 0;

  template <typename Emit> DrainOutcome operator()(const Emit& emit) const {
    DrainOutcome outcome;
    std::array<char, 4096> buffer{};

    while (outcome.bytes_read < budget) {
      const std::size_t want = std::min(buffer.size(), budget - outcome.bytes_read);
      const ssize_t n = ::read(fd, buffer.data(), want);
      if (n > 0) {
        const auto count = static_cast<std::size_t>(n);
        outcome.bytes_read += count;
        emit(std::string_view(buffer.data(), count));
        continue;
      }
      if (n == 0) {
        // Peer closed the write end.
        outcome.closed = true;
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        break;
      }
      outcome.closed = true;
      break;
    }

    return outcome;
  }
};

// Buffers everything a one-shot child writes, which is what RunOnce needs and what a streaming
// caller does not: streaming callers consume chunks incrementally through their own sink.
class RunSink final : public PumpSink {
public:
  void OnStdout(std::string_view chunk) override { stdout_text.append(chunk); }
  void OnStderr(std::string_view chunk) override { stderr_text.append(chunk); }

  std::string stdout_text;
  std::string stderr_text;
};

} // namespace

bool KillAndReap(pid_t pid) {
  if (pid <= 0) {
    return true;
  }
  constexpr int kSigKill = 9; // POSIX SIGKILL
  (void)syscall(SYS_kill, pid, kSigKill);
  pid_t wait_rc = waitpid(pid, nullptr, 0);
  while (wait_rc < 0 && errno == EINTR) {
    wait_rc = waitpid(pid, nullptr, 0);
  }
  return wait_rc == pid || (wait_rc < 0 && errno == ECHILD);
}

bool TryReapProcess(pid_t pid, int* exit_code) {
  if (pid <= 0) {
    return false;
  }
  int status = 0;
  constexpr int kNoHang = 1; // POSIX WNOHANG
  const pid_t rc = waitpid(pid, &status, kNoHang);
  if (rc != pid) {
    return false;
  }
  if (exit_code != nullptr) {
    const int term_sig = status & 0x7f;
    if (term_sig == 0) {
      *exit_code = (status >> 8) & 0xff; // WEXITSTATUS
    } else {
      *exit_code = 128 + term_sig; // WTERMSIG
    }
  }
  return true;
}

void TerminateProcessBounded(pid_t pid, std::chrono::milliseconds graceful_timeout,
                             std::chrono::milliseconds force_timeout) {
  if (pid <= 0) {
    return;
  }

  // Reaps the child within `timeout`. ECHILD means it was already reaped
  // elsewhere (or is not our child), so there is nothing left to wait for.
  const auto wait_for_exit = [pid](std::chrono::milliseconds timeout) {
    constexpr auto kPollInterval = std::chrono::milliseconds(100);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      errno = 0;
      if (TryReapProcess(pid) || errno == ECHILD) {
        return true;
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    return TryReapProcess(pid);
  };

  (void)syscall(SYS_kill, pid, SIGTERM);
  if (wait_for_exit(graceful_timeout)) {
    return;
  }

  (void)syscall(SYS_kill, pid, SIGKILL);
  (void)wait_for_exit(force_timeout);
}

bool CreateCloexecPipe(int pipefd[2]) {
#if defined(__linux__)
  return pipe2(pipefd, O_CLOEXEC) == 0;
#else
  if (pipe(pipefd) != 0) {
    return false;
  }
  (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
  (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  return true;
#endif
}

PumpResult PumpIo(const PumpChild& child, std::span<const std::byte> input,
                  const PumpOptions& options, PumpSink& sink) {
  using Clock = std::chrono::steady_clock;

  PumpResult result;
  const int in_fd = child.channels.input_fd;
  const int out_fd = child.channels.stdout_fd;
  const int err_fd = child.channels.stderr_fd;

  std::size_t input_offset = 0;
  bool in_open = (in_fd >= 0 && !input.empty());
  bool out_open = (out_fd >= 0);
  bool err_open = (err_fd >= 0);
  bool input_closed = false;

  // Marks the input channel done and lets the owner release its own handle exactly once.
  const auto finish_input = [&]() {
    in_open = false;
    if (!input_closed) {
      input_closed = true;
      if (options.close_input_after_write && child.close_input) {
        child.close_input();
      }
    }
  };

  if (in_fd >= 0 && input.empty()) {
    finish_input();
  }

  std::optional<Clock::time_point> child_exited_at;

  while (in_open || out_open || err_open || !result.child_exited) {
    if (!result.child_exited && child.exited) {
      int exit_code = -1;
      if (child.exited(&exit_code)) {
        result.child_exited = true;
        result.child_exit_code = exit_code;
        child_exited_at = Clock::now();
        if (in_open) {
          // The child is gone, so the rest of the input can never be read.
          result.input_incomplete = true;
          finish_input();
        }
      }
    }

    const auto now = Clock::now();
    if (now >= options.deadline) {
      result.timed_out = true;
      if (in_open) {
        result.input_incomplete = true;
        in_open = false;
      }
      break;
    }

    // A closed child can still leave buffered output behind, so drain for a bounded grace only.
    if (result.child_exited && child_exited_at.has_value() &&
        now - *child_exited_at >= options.post_exit_grace) {
      break;
    }

    std::array<pollfd, 3> pfds{};
    std::size_t pfd_count = 0;
    int in_idx = -1;
    int out_idx = -1;
    int err_idx = -1;

    if (in_open) {
      in_idx = static_cast<int>(pfd_count);
      pfds.at(pfd_count) = pollfd{.fd = in_fd, .events = POLLOUT, .revents = 0};
      pfd_count += 1;
    }
    if (out_open) {
      out_idx = static_cast<int>(pfd_count);
      pfds.at(pfd_count) = pollfd{.fd = out_fd, .events = POLLIN, .revents = 0};
      pfd_count += 1;
    }
    if (err_open) {
      err_idx = static_cast<int>(pfd_count);
      pfds.at(pfd_count) = pollfd{.fd = err_fd, .events = POLLIN, .revents = 0};
      pfd_count += 1;
    }

    auto wait_ms = static_cast<int>(std::clamp<std::int64_t>(options.poll_slice.count(), 0, 1000));
    if (options.deadline != Clock::time_point::max()) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline - now).count();
      wait_ms = std::min(wait_ms, static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 1000)));
    }

    if (pfd_count == 0) {
      // Every channel is closed. Without an exit probe nothing else can be observed, so stop: an
      // fd-only pump has no further state to wait for.
      if (!child.exited) {
        break;
      }
      // With a probe the child still runs: keep the deadline in force instead of handing the caller
      // an unbounded wait, and sleep rather than spin while doing it.
      std::this_thread::sleep_for(std::chrono::milliseconds(std::max(wait_ms, 1)));
      continue;
    }

    const int poll_rc = ::poll(pfds.data(), static_cast<nfds_t>(pfd_count), wait_ms);
    if (poll_rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (poll_rc == 0) {
      continue;
    }

    if (in_idx >= 0) {
      const auto revents = pfds.at(static_cast<std::size_t>(in_idx)).revents;
      if ((revents & POLLOUT) != 0) {
        const std::size_t remaining = input.size() - input_offset;
        const ssize_t written = ::write(in_fd, input.data() + input_offset, remaining);
        if (written > 0) {
          input_offset += static_cast<std::size_t>(written);
          if (input_offset >= input.size()) {
            finish_input();
          }
        } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          result.input_incomplete = true;
          finish_input();
        }
      } else if ((revents & (POLLHUP | POLLERR)) != 0) {
        result.input_incomplete = true;
        finish_input();
      }
    }

    // A zero budget would leave readable data in the pipe and spin the loop, so keep it non-zero.
    std::size_t budget = std::max<std::size_t>(options.read_budget_bytes, 1);

    if (out_open && out_idx >= 0) {
      const auto revents = pfds.at(static_cast<std::size_t>(out_idx)).revents;
      if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        const DrainOutcome drained = DrainFdBudgeted{.fd = out_fd, .budget = budget}(
            [&sink](std::string_view chunk) { sink.OnStdout(chunk); });
        budget -= std::min(budget, drained.bytes_read);
        if (drained.closed) {
          out_open = false;
          result.stdout_closed = true;
        }
      }
    }

    if (err_open && err_idx >= 0) {
      const auto revents = pfds.at(static_cast<std::size_t>(err_idx)).revents;
      if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        const DrainOutcome drained = DrainFdBudgeted{.fd = err_fd, .budget = budget}(
            [&sink](std::string_view chunk) { sink.OnStderr(chunk); });
        if (drained.closed) {
          err_open = false;
          result.stderr_closed = true;
        }
      }
    }
  }

  // The child may have exited while the last poll window was still open.
  if (!result.child_exited && child.exited) {
    int exit_code = -1;
    if (child.exited(&exit_code)) {
      result.child_exited = true;
      result.child_exit_code = exit_code;
    }
  }

  return result;
}

CommandResult RunOnce(const CommandSpec& spec, std::span<const std::byte> input) {
  CommandResult result;
  if (spec.command.empty()) {
    result.launch_failed = true;
    result.stderr_text = "Command is empty.";
    return result;
  }

  const auto cmd = BuildCmd(spec);
  const auto env_map = BuildEnvMap(spec);

  try {
    auto proc =
        subprocess::Popen(cmd, subprocess::input{subprocess::PIPE},
                          subprocess::output{subprocess::PIPE}, subprocess::error{subprocess::PIPE},
                          subprocess::environment{env_map}, subprocess::session_leader{true});

    const int in_fd = proc.input() != nullptr ? subprocess_fileno(proc.input()) : -1;
    const int out_fd = proc.output() != nullptr ? subprocess_fileno(proc.output()) : -1;
    const int err_fd = proc.error() != nullptr ? subprocess_fileno(proc.error()) : -1;

    if (in_fd >= 0) {
      SetNonBlocking(in_fd);
    }
    if (out_fd >= 0) {
      SetNonBlocking(out_fd);
    }
    if (err_fd >= 0) {
      SetNonBlocking(err_fd);
    }

    PumpChild child;
    child.channels = PumpChannels{.input_fd = in_fd, .stdout_fd = out_fd, .stderr_fd = err_fd};
    child.exited = [&proc](int* exit_code) {
      const int poll_rc = proc.poll();
      if (poll_rc == -1) {
        return false;
      }
      if (exit_code != nullptr) {
        *exit_code = poll_rc;
      }
      return true;
    };
    child.close_input = [&proc] { proc.close_input(); };

    PumpOptions options;
    if (spec.timeout_ms > 0) {
      options.deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeout_ms);
    }

    RunSink sink;
    const PumpResult pump = PumpIo(child, input, options, sink);

    result.timed_out = pump.timed_out;
    result.stdout_text = std::move(sink.stdout_text);
    result.stderr_text = std::move(sink.stderr_text);

    if (pump.timed_out) {
      proc.kill();
      result.exit_code = -1;
    } else if (pump.child_exited) {
      result.exit_code = pump.child_exit_code;
    } else {
      // Every channel closed without a confirmed exit, so wait for the child itself.
      result.exit_code = proc.wait();
    }
    return result;
  } catch (const subprocess::OSError& e) {
    result.launch_failed = true;
    result.stderr_text = e.what();
    return result;
  } catch (const std::exception& e) {
    result.launch_failed = true;
    result.stderr_text = e.what();
    return result;
  }
}

Expected<SpawnedProcess> SpawnForMonitoring(const CommandSpec& spec,
                                            const std::filesystem::path& working_dir) {
  if (spec.command.empty()) {
    return UnexpectedError("Command is empty.");
  }

  const auto cmd = BuildCmd(spec);
  const auto env_map = BuildEnvMap(spec);

  try {
    auto proc = subprocess::Popen(
        cmd, subprocess::input{"/dev/null"}, subprocess::output{"/dev/null"},
        subprocess::error{subprocess::PIPE}, subprocess::cwd{working_dir.string()},
        subprocess::environment{env_map}, subprocess::session_leader{true});

    SpawnedProcess process_out{.pid = proc.pid(), .stderr_fd = -1};
    FILE* err_fp = proc.error();
    if (err_fp != nullptr) {
      const int dup_fd = fcntl(subprocess_fileno(err_fp), F_DUPFD_CLOEXEC, 0);
      if (dup_fd < 0) {
        const std::string err =
            "fcntl F_DUPFD_CLOEXEC failed: " + std::string(std::strerror(errno));
        proc.kill();
        return UnexpectedError(err);
      }
      process_out.stderr_fd = dup_fd;
    }
    return process_out;
  } catch (const std::exception& e) {
    return UnexpectedError(e.what());
  }
}

bool RunCommandSilent(const CommandSpec& spec, int* exit_code) {
  if (spec.command.empty()) {
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return false;
  }

  const auto result = RunOnce(spec, {});
  if (exit_code != nullptr) {
    *exit_code = result.exit_code;
  }
  return !result.launch_failed && !result.timed_out && result.exit_code == 0;
}

Expected<std::string> RunCommandCaptureStdout(const CommandSpec& spec) {
  if (spec.command.empty()) {
    return UnexpectedError("Command is empty.");
  }

  const auto result = RunOnce(spec, {});
  if (result.launch_failed) {
    return UnexpectedError(!result.stderr_text.empty() ? result.stderr_text
                                                       : "Failed to launch process.");
  }
  if (result.timed_out) {
    return UnexpectedError("Process timed out.");
  }
  if (result.exit_code != 0) {
    const std::string err = !result.stderr_text.empty()
                                ? result.stderr_text
                                : "Process exited with code " + std::to_string(result.exit_code);
    return UnexpectedError(err);
  }
  return result.stdout_text;
}

} // namespace vinput::process
