#include "common/utils/process_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <subprocess.hpp>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <signal.h> // SIGKILL
#include <time.h>   // pid_t
}

#include "common/utils/debug_log.h"
#include "common/utils/expected.h"
#include "common/utils/interpolation.h"
#include "common/utils/process_internal.h"

namespace vinput::process {

namespace detail {

void EnsureSigpipeIgnored() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
      VLOG_WARN("process: SIGPIPE could not be ignored; a pipe write may end the process");
    }
  });
}

bool MakeNonBlocking(int fd) {
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

} // namespace detail

namespace {

constexpr int kPollSliceMs = 50;
constexpr auto kPostExitGrace = std::chrono::milliseconds(200);

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
  const auto make_non_blocking = [](int fd) { return fd < 0 || detail::MakeNonBlocking(fd); };
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
      } else if ((revents & detail::kInputFailureEvents) != 0) {
        outcome.input_incomplete = true;
        finish_input();
      }
    }

    if (out_open && out_idx >= 0 &&
        (pfds.at(static_cast<std::size_t>(out_idx)).revents & detail::kReadableChannelEvents) !=
            0) {
      const detail::DrainOutcome drained = detail::DrainBudgeted(
          out_fd, detail::kReadBudgetBytes,
          [&stdout_text](std::string_view chunk) { stdout_text.append(chunk); });
      out_open = !drained.closed;
    }

    if (err_open && err_idx >= 0 &&
        (pfds.at(static_cast<std::size_t>(err_idx)).revents & detail::kReadableChannelEvents) !=
            0) {
      const detail::DrainOutcome drained = detail::DrainBudgeted(
          err_fd, detail::kReadBudgetBytes,
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

// Each probe of a pinned process costs one poll() call, so the wait is paced instead of spun.
constexpr auto kTerminateProbeInterval = std::chrono::milliseconds(100);

// An interrupted or failed probe says nothing about the process, so it is retried a few times
// before being reported as undetermined.
constexpr int kIdentityProbeAttempts = 3;

bool SendSignalByPid(pid_t pid, int signal_number) {
#if defined(SYS_kill)
  // The kill syscall is preferred over tgkill because it keeps the signal process-directed:
  // tgkill(pid, pid, ...) would only reach the thread-group leader, so a process that handles
  // termination on another thread could swallow the request.
  return ::syscall(SYS_kill, pid, signal_number) == 0;
#else
  (void)pid;
  (void)signal_number;
  errno = ENOSYS;
  return false;
#endif
}

// pidfd_open/pidfd_send_signal are Linux 5.3/5.1. Referencing a process through a pidfd keeps the
// kernel object alive, so a signal can never be delivered to a recycled pid. The syscall numbers
// come from the kernel headers, so guard on their presence to stay buildable on older toolchains.
int OpenPidFd(pid_t pid) {
#if defined(SYS_pidfd_open)
  return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
  (void)pid;
  errno = ENOSYS;
  return -1;
#endif
}

bool SendSignalViaPidFd(int pidfd, int signal_number) {
#if defined(SYS_pidfd_send_signal)
  return ::syscall(SYS_pidfd_send_signal, pidfd, signal_number, nullptr, 0) == 0;
#else
  (void)pidfd;
  (void)signal_number;
  errno = ENOSYS;
  return false;
#endif
}

// How a pinned process was observed when its descriptor was probed.
enum class ProcessState : std::uint8_t { Running, Exited, Unknown };

// poll() reports readable as soon as the process exits, even before it is reaped. An interrupted or
// failed probe says nothing about the process, so it is reported as Unknown rather than being taken
// for either outcome.
ProcessState ProbePinnedProcess(int pidfd) {
  pollfd descriptor{};
  descriptor.fd = pidfd;
  descriptor.events = POLLIN;
  for (int attempt = 0; attempt < kIdentityProbeAttempts; ++attempt) {
    const int rc = ::poll(&descriptor, 1, 0);
    if (rc > 0) {
      return (descriptor.revents & POLLIN) != 0 ? ProcessState::Exited : ProcessState::Unknown;
    }
    if (rc == 0) {
      return ProcessState::Running;
    }
    if (errno != EINTR) {
      return ProcessState::Unknown;
    }
  }
  return ProcessState::Unknown;
}

// Outcome of waiting for a process to exit.
enum class ExitWait : std::uint8_t { Exited, StillRunning, Inconclusive };

// Result of delivering a signal to a process identified only by its pid.
enum class SignalOutcome : std::uint8_t { Delivered, ProcessGone, Failed };

// One specific process instance, addressed either through a pidfd or, when the kernel has none, by
// its pid with the recorded identity re-checked before every signal. Holding the descriptor keeps
// this exact instance referenced, which closes the pid-reuse window entirely; without one only the
// gap between the check and the signal remains.
class PinnedProcess {
public:
  PinnedProcess(pid_t pid, std::uint64_t start_time) : pid_(pid), start_time_(start_time) {
    if (pid_ > 0 && start_time_ != 0) {
      pidfd_ = OpenPidFd(pid_);
      if (pidfd_ < 0) {
        pin_errno_ = errno;
      }
    }
  }

  PinnedProcess(const PinnedProcess&) = delete;
  PinnedProcess& operator=(const PinnedProcess&) = delete;
  PinnedProcess(PinnedProcess&&) = delete;
  PinnedProcess& operator=(PinnedProcess&&) = delete;

  ~PinnedProcess() {
    if (pidfd_ >= 0) {
      (void)close(pidfd_);
    }
  }

  // True when opening the descriptor failed because nothing runs at that pid any more.
  [[nodiscard]] bool PinObservedNoSuchProcess() const { return pidfd_ < 0 && pin_errno_ == ESRCH; }

  // True when the live process is still the recorded instance.
  [[nodiscard]] bool Matches() const { return ProcessIdentityMatches(pid_, start_time_); }

  [[nodiscard]] SignalOutcome Signal(int signal_number) const {
    if (pidfd_ >= 0) {
      while (true) {
        if (SendSignalViaPidFd(pidfd_, signal_number)) {
          return SignalOutcome::Delivered;
        }
        if (errno != EINTR) {
          break;
        }
      }
    } else {
      // No descriptor pins the instance, so the identity is re-confirmed immediately before the
      // signal: a pid recycled earlier cannot be reached.
      if (!Matches()) {
        return SignalOutcome::ProcessGone;
      }
      if (SendSignalByPid(pid_, signal_number)) {
        return SignalOutcome::Delivered;
      }
    }
    return errno == ESRCH ? SignalOutcome::ProcessGone : SignalOutcome::Failed;
  }

  [[nodiscard]] ExitWait WaitForExit(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool observed_running = false;
    while (true) {
      const ProcessState state = Probe();
      if (state == ProcessState::Exited) {
        return ExitWait::Exited;
      }
      if (state == ProcessState::Running) {
        observed_running = true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        // Only a readable descriptor - or a vanished identity - confirms an exit, so running out of
        // time is never folded into one of those outcomes.
        return observed_running ? ExitWait::StillRunning : ExitWait::Inconclusive;
      }
      std::this_thread::sleep_for(kTerminateProbeInterval);
    }
  }

private:
  // Without a descriptor only the recorded identity can tell the instance apart, so a vanished
  // identity is reported as an exit and a matching one as still running.
  [[nodiscard]] ProcessState Probe() const {
    if (pidfd_ >= 0) {
      return ProbePinnedProcess(pidfd_);
    }
    return Matches() ? ProcessState::Running : ProcessState::Exited;
  }

  pid_t pid_ = -1;
  std::uint64_t start_time_ = 0;
  int pidfd_ = -1;
  int pin_errno_ = 0;
};

} // namespace

std::uint64_t ProcessStartTime(pid_t pid) {
  if (pid <= 0) {
    return 0;
  }
  std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
  if (!stat_file.is_open()) {
    return 0;
  }
  std::string line;
  if (!std::getline(stat_file, line)) {
    return 0;
  }
  // Field 2 is the executable name wrapped in parentheses and may itself contain spaces or
  // parentheses, so resume parsing after the last ')'.
  const std::size_t comm_end = line.rfind(')');
  constexpr std::size_t kFieldsBeforeResume = 2;
  if (comm_end == std::string::npos || comm_end + kFieldsBeforeResume >= line.size()) {
    return 0;
  }
  constexpr int kStartTimeField = 22;
  std::istringstream fields(line.substr(comm_end + kFieldsBeforeResume));
  std::string token;
  for (int field = 3; field <= kStartTimeField; ++field) {
    if (!(fields >> token)) {
      return 0;
    }
  }
  std::uint64_t start_time = 0;
  const char* const begin = token.data();
  const char* const end = token.data() + token.size();
  const auto parsed = std::from_chars(begin, end, start_time);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return 0;
  }
  return start_time;
}

bool ProcessIdentityMatches(pid_t pid, std::uint64_t start_time) {
  if (pid <= 0 || start_time == 0) {
    return false;
  }
  return ProcessStartTime(pid) == start_time;
}

TerminateResult TerminateGracefully(pid_t pid, std::uint64_t start_time,
                                    std::chrono::milliseconds graceful_timeout,
                                    std::chrono::milliseconds force_timeout) {
  TerminateResult result;
  if (pid <= 0 || start_time == 0) {
    // A record without an identity cannot be re-identified, so it is never trusted with a signal.
    result.outcome = TerminateOutcome::NotFound;
    return result;
  }

  const PinnedProcess process(pid, start_time);
  if (process.PinObservedNoSuchProcess() || !process.Matches()) {
    // Either the process already exited, or the pid was recycled by an unrelated one. Neither may
    // be signalled, so the caller is told there is nothing left to stop.
    result.outcome = TerminateOutcome::NotFound;
    return result;
  }

  const SignalOutcome terminate = process.Signal(SIGTERM);
  if (terminate == SignalOutcome::ProcessGone) {
    result.outcome = TerminateOutcome::NotFound;
    return result;
  }
  if (terminate == SignalOutcome::Failed) {
    result.failed_signal = SIGTERM;
    result.error = std::strerror(errno);
    return result;
  }
  if (process.WaitForExit(graceful_timeout) == ExitWait::Exited) {
    result.outcome = TerminateOutcome::Exited;
    return result;
  }

  const SignalOutcome forced = process.Signal(SIGKILL);
  if (forced == SignalOutcome::Failed) {
    result.failed_signal = SIGKILL;
    result.error = std::strerror(errno);
    return result;
  }
  // A process that vanished on its own between the two signals is already the exit we wanted.
  const ExitWait final_wait =
      forced == SignalOutcome::ProcessGone ? ExitWait::Exited : process.WaitForExit(force_timeout);

  switch (final_wait) {
  case ExitWait::Exited:
    result.outcome = TerminateOutcome::Exited;
    break;
  case ExitWait::StillRunning:
    result.outcome = TerminateOutcome::StillRunning;
    break;
  case ExitWait::Inconclusive:
    result.outcome = TerminateOutcome::Unconfirmed;
    break;
  }
  return result;
}

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
  detail::EnsureSigpipeIgnored();

  CommandResult result;
  if (spec.command.empty()) {
    result.launch_failed = true;
    result.stderr_text = "Command is empty.";
    return result;
  }

  const auto cmd = detail::BuildCmd(spec);
  const auto env_map = detail::BuildEnvMap(spec);

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

  const auto cmd = detail::BuildCmd(spec);
  const auto env_map = detail::BuildEnvMap(spec);

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
