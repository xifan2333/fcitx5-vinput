#include "common/utils/process_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
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

CommandResult RunCommandWithInput(const CommandSpec& spec, std::span<const std::byte> input) {
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

    const auto start_time = std::chrono::steady_clock::now();
    const auto deadline = spec.timeout_ms > 0
                              ? start_time + std::chrono::milliseconds(spec.timeout_ms)
                              : std::chrono::steady_clock::time_point::max();

    std::size_t input_offset = 0;
    bool in_open = (in_fd >= 0 && !input.empty());
    bool out_open = (out_fd >= 0);
    bool err_open = (err_fd >= 0);

    if (in_fd >= 0 && input.empty()) {
      proc.close_input();
      in_open = false;
    }

    bool child_exited = false;
    int child_exit_code = -1;
    std::optional<std::chrono::steady_clock::time_point> child_exited_at;

    std::array<char, 4096> drain_buffer{};

    while (in_open || out_open || err_open || !child_exited) {
      if (!child_exited) {
        const int poll_res = proc.poll();
        if (poll_res != -1) {
          child_exited = true;
          child_exit_code = poll_res;
          child_exited_at = std::chrono::steady_clock::now();
          if (in_open) {
            proc.close_input();
            in_open = false;
          }
        }
      }

      const auto now = std::chrono::steady_clock::now();
      if (spec.timeout_ms > 0 && now >= deadline) {
        result.timed_out = true;
        proc.kill();
        break;
      }

      if (child_exited && child_exited_at.has_value()) {
        constexpr auto kPostExitGrace = std::chrono::milliseconds(200);
        if (now - child_exited_at.value() >= kPostExitGrace) {
          break;
        }
      }

      std::vector<pollfd> pfds;
      pfds.reserve(3);

      int in_idx = -1;
      int out_idx = -1;
      int err_idx = -1;

      if (in_open && in_fd >= 0) {
        in_idx = static_cast<int>(pfds.size());
        pollfd pfd{};
        pfd.fd = in_fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        pfds.push_back(pfd);
      }
      if (out_open && out_fd >= 0) {
        out_idx = static_cast<int>(pfds.size());
        pollfd pfd{};
        pfd.fd = out_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pfds.push_back(pfd);
      }
      if (err_open && err_fd >= 0) {
        err_idx = static_cast<int>(pfds.size());
        pollfd pfd{};
        pfd.fd = err_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pfds.push_back(pfd);
      }

      if (pfds.empty()) {
        if (!child_exited) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        break;
      }

      int wait_ms = 50;
      if (spec.timeout_ms > 0) {
        const auto rem =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        wait_ms = std::clamp<int>(static_cast<int>(rem), 0, 50);
      }

      const int poll_rc = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), wait_ms);
      if (poll_rc < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      if (in_idx >= 0 && (pfds[static_cast<std::size_t>(in_idx)].revents & POLLOUT) != 0) {
        const std::size_t remaining = input.size() - input_offset;
        if (remaining == 0) {
          proc.close_input();
          in_open = false;
        } else {
          const ssize_t written = write(in_fd, input.data() + input_offset, remaining);
          if (written > 0) {
            input_offset += static_cast<std::size_t>(written);
            if (input_offset >= input.size()) {
              proc.close_input();
              in_open = false;
            }
          } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            proc.close_input();
            in_open = false;
          }
        }
      }

      if (out_idx >= 0) {
        const auto rev = pfds[static_cast<std::size_t>(out_idx)].revents;
        if ((rev & POLLIN) != 0) {
          while (true) {
            const ssize_t n = read(out_fd, drain_buffer.data(), drain_buffer.size());
            if (n > 0) {
              result.stdout_text.append(drain_buffer.data(), static_cast<std::size_t>(n));
            } else if (n == 0) {
              out_open = false;
              break;
            } else {
              break;
            }
          }
        } else if ((rev & (POLLHUP | POLLERR)) != 0) {
          while (true) {
            const ssize_t n = read(out_fd, drain_buffer.data(), drain_buffer.size());
            if (n > 0) {
              result.stdout_text.append(drain_buffer.data(), static_cast<std::size_t>(n));
            } else {
              break;
            }
          }
          out_open = false;
        }
      }

      if (err_idx >= 0) {
        const auto rev = pfds[static_cast<std::size_t>(err_idx)].revents;
        if ((rev & POLLIN) != 0) {
          while (true) {
            const ssize_t n = read(err_fd, drain_buffer.data(), drain_buffer.size());
            if (n > 0) {
              result.stderr_text.append(drain_buffer.data(), static_cast<std::size_t>(n));
            } else if (n == 0) {
              err_open = false;
              break;
            } else {
              break;
            }
          }
        } else if ((rev & (POLLHUP | POLLERR)) != 0) {
          while (true) {
            const ssize_t n = read(err_fd, drain_buffer.data(), drain_buffer.size());
            if (n > 0) {
              result.stderr_text.append(drain_buffer.data(), static_cast<std::size_t>(n));
            } else {
              break;
            }
          }
          err_open = false;
        }
      }
    }

    if (result.timed_out) {
      result.exit_code = -1;
    } else if (child_exited) {
      result.exit_code = child_exit_code;
    } else {
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

  const auto result = RunCommandWithInput(spec, {});
  if (exit_code != nullptr) {
    *exit_code = result.exit_code;
  }
  return !result.launch_failed && !result.timed_out && result.exit_code == 0;
}

Expected<std::string> RunCommandCaptureStdout(const CommandSpec& spec) {
  if (spec.command.empty()) {
    return UnexpectedError("Command is empty.");
  }

  const auto result = RunCommandWithInput(spec, {});
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
