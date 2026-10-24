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

// Events that mean a read is worth attempting on a channel: data, a closed peer, a broken pipe, or
// a descriptor that died while the pump was running. POLLNVAL has to be here because it is the only
// way an invalid descriptor gets closed: the read that follows fails with EBADF and reports the
// channel closed. Without it the channel stays open and poll() returns at once on every iteration,
// burning the CPU until the deadline.
constexpr short kReadableChannelEvents = POLLIN | POLLHUP | POLLERR | POLLNVAL;

// Events that end the input channel without a successful write, including an invalid descriptor.
constexpr short kInputFailureEvents = POLLHUP | POLLERR | POLLNVAL;

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

constexpr int kPollSliceMs = 50;
constexpr auto kPostExitGrace = std::chrono::milliseconds(200);

// Every channel gets its own budget. Nothing is shared between stdout and stderr, so a child that
// floods one stream cannot hold the other back: there is no quota to split, no ordering rule and no
// starvation to compensate for. Starvation would not just be slow - once the starved pipe fills,
// the child blocks in write() and both directions stall until the deadline.
constexpr std::size_t kReadBudgetBytes = std::size_t{64} * 1024;

struct PumpOutcome {
  bool timed_out = false;
  bool input_incomplete = false;
  bool child_exited = false;
  int child_exit_code = -1;
};

// Drives one child until it exits or `deadline` expires: feeds `input` to its stdin, appends
// everything it prints to `stdout_text` / `stderr_text`, then reports how it ended.
//
// This is the module's only pipe driver, and it is deliberately not part of the interface: every
// caller wants "run this provider command", never "pump these descriptors". Descriptors are forced
// non-blocking, because a blocking read or write would overshoot the deadline, and the stdin handle
// is released before returning so a child that only exits on EOF never waits on a handle that is
// still held here.
PumpOutcome PumpProcess(subprocess::Popen& proc, std::span<const std::byte> input,
                        std::chrono::steady_clock::time_point deadline, std::string& stdout_text,
                        std::string& stderr_text) {
  using Clock = std::chrono::steady_clock;

  PumpOutcome outcome;
  const int in_fd = proc.input() != nullptr ? subprocess_fileno(proc.input()) : -1;
  const int out_fd = proc.output() != nullptr ? subprocess_fileno(proc.output()) : -1;
  const int err_fd = proc.error() != nullptr ? subprocess_fileno(proc.error()) : -1;

  // poll() only announces readiness, so nothing below may block. A descriptor that refuses to
  // switch is left out of the poll set instead of being read while it can still block.
  const auto make_non_blocking = [](int fd) { return fd < 0 || SetNonBlocking(fd); };
  const bool in_usable = make_non_blocking(in_fd);
  const bool out_usable = make_non_blocking(out_fd);
  const bool err_usable = make_non_blocking(err_fd);

  std::size_t input_offset = 0;
  bool in_open = (in_fd >= 0 && in_usable && !input.empty());
  bool out_open = (out_fd >= 0 && out_usable);
  bool err_open = (err_fd >= 0 && err_usable);
  bool input_closed = false;
  outcome.input_incomplete = !in_usable;

  // Releases the caller's stdin handle exactly once, however this pump ends.
  const auto finish_input = [&]() {
    in_open = false;
    if (!input_closed) {
      input_closed = true;
      proc.close_input();
    }
  };

  if (in_fd >= 0 && input.empty()) {
    finish_input();
  }

  std::optional<Clock::time_point> child_exited_at;

  while (in_open || out_open || err_open || !outcome.child_exited) {
    if (!outcome.child_exited) {
      const int exit_code = proc.poll();
      if (exit_code != -1) {
        outcome.child_exited = true;
        outcome.child_exit_code = exit_code;
        child_exited_at = Clock::now();
        if (in_open) {
          // The child is gone, so the rest of the input can never be read.
          outcome.input_incomplete = true;
          finish_input();
        }
      }
    }

    const auto now = Clock::now();
    if (now >= deadline) {
      outcome.timed_out = true;
      if (in_open) {
        outcome.input_incomplete = true;
        in_open = false;
      }
      break;
    }

    // A closed child can still leave buffered output behind, so drain for a bounded grace only.
    if (outcome.child_exited && child_exited_at.has_value() &&
        now - *child_exited_at >= kPostExitGrace) {
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

    auto wait_ms = std::clamp<int>(kPollSliceMs, 0, 1000);
    if (deadline != Clock::time_point::max()) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      wait_ms = std::min(wait_ms, static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 1000)));
    }

    if (pfd_count == 0) {
      // Every channel is closed but the child still runs: keep the deadline in force instead of
      // handing the caller an unbounded wait, and sleep rather than spin while doing it.
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
          outcome.input_incomplete = true;
          finish_input();
        }
      } else if ((revents & kInputFailureEvents) != 0) {
        outcome.input_incomplete = true;
        finish_input();
      }
    }

    if (out_open && out_idx >= 0 &&
        (pfds.at(static_cast<std::size_t>(out_idx)).revents & kReadableChannelEvents) != 0) {
      const DrainOutcome drained = DrainFdBudgeted{.fd = out_fd, .budget = kReadBudgetBytes}(
          [&stdout_text](std::string_view chunk) { stdout_text.append(chunk); });
      out_open = !drained.closed;
    }

    if (err_open && err_idx >= 0 &&
        (pfds.at(static_cast<std::size_t>(err_idx)).revents & kReadableChannelEvents) != 0) {
      const DrainOutcome drained = DrainFdBudgeted{.fd = err_fd, .budget = kReadBudgetBytes}(
          [&stderr_text](std::string_view chunk) { stderr_text.append(chunk); });
      err_open = !drained.closed;
    }
  }

  // The child may have exited while the last poll window was still open.
  if (!outcome.child_exited) {
    const int exit_code = proc.poll();
    if (exit_code != -1) {
      outcome.child_exited = true;
      outcome.child_exit_code = exit_code;
    }
  }

  // However the loop ended - deadline, poll failure, unusable descriptor - the child must never be
  // left waiting for EOF on a stdin this function still holds.
  if (in_fd >= 0) {
    finish_input();
  }

  return outcome;
}

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

CommandResult Run(const CommandSpec& spec, std::span<const std::byte> stdin_bytes) {
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

    const auto deadline = spec.timeout_ms > 0 ? std::chrono::steady_clock::now() +
                                                    std::chrono::milliseconds(spec.timeout_ms)
                                              : std::chrono::steady_clock::time_point::max();
    const PumpOutcome outcome =
        PumpProcess(proc, stdin_bytes, deadline, result.stdout_text, result.stderr_text);

    result.timed_out = outcome.timed_out;
    if (outcome.timed_out) {
      proc.kill();
      // The child was killed because the deadline expired, but kill() only sends the signal: the
      // zombie must be reaped here or every timed-out Run() leaks one process (Popen's destructor
      // is disabled upstream). wait() is safe because the child was just SIGKILLed.
      (void)proc.wait();
      result.exit_code = -1;
    } else if (outcome.child_exited) {
      result.exit_code = outcome.child_exit_code;
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

} // namespace vinput::process
