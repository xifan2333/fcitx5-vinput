#include "common/utils/process_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "common/utils/interpolation.h"
#include "common/utils/time_utils.h"

extern char** environ;

namespace vinput::process {

namespace {

constexpr int kWaitNoHang = 1;

bool SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::vector<char*> BuildArgv(const CommandSpec& spec, std::vector<std::string>* storage) {
  storage->clear();
  storage->reserve(spec.args.size() + 1);
  storage->push_back(spec.command);
  for (const auto& arg : spec.args) {
    storage->push_back(arg);
  }

  std::vector<char*> argv;
  argv.reserve(storage->size() + 1);
  for (auto& arg : *storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return argv;
}

std::vector<char*> BuildEnvp(const CommandSpec& spec, std::vector<std::string>* storage) {
  storage->clear();

  for (char** env = environ; env && *env; ++env) {
    storage->push_back(*env);
  }

  for (const auto& [key, value] : spec.env) {
    bool replaced = false;
    const std::string resolved_value = vinput::interpolate::InterpolateEnv(value);
    const std::string prefix = key + "=";
    for (auto& entry : *storage) {
      if (entry.rfind(prefix, 0) == 0) {
        entry = prefix + resolved_value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      storage->push_back(prefix + resolved_value);
    }
  }

  std::vector<char*> envp;
  envp.reserve(storage->size() + 1);
  for (auto& entry : *storage) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);
  return envp;
}

void CloseIfOpen(int* fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

} // namespace

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

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (!CreateCloexecPipe(stdin_pipe) || !CreateCloexecPipe(stdout_pipe) ||
      !CreateCloexecPipe(stderr_pipe)) {
    result.launch_failed = true;
    result.stderr_text = std::string("pipe failed: ") + std::strerror(errno);
    CloseIfOpen(&stdin_pipe[0]);
    CloseIfOpen(&stdin_pipe[1]);
    CloseIfOpen(&stdout_pipe[0]);
    CloseIfOpen(&stdout_pipe[1]);
    CloseIfOpen(&stderr_pipe[0]);
    CloseIfOpen(&stderr_pipe[1]);
    return result;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);

  std::vector<std::string> argv_storage;
  std::vector<char*> argv = BuildArgv(spec, &argv_storage);
  std::vector<std::string> env_storage;
  std::vector<char*> envp = BuildEnvp(spec, &env_storage);

  pid_t pid = -1;
  const int spawn_rc =
      posix_spawnp(&pid, spec.command.c_str(), &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);

  CloseIfOpen(&stdin_pipe[0]);
  CloseIfOpen(&stdout_pipe[1]);
  CloseIfOpen(&stderr_pipe[1]);

  if (spawn_rc != 0) {
    result.launch_failed = true;
    result.stderr_text = std::string("failed to launch command: ") + std::strerror(spawn_rc);
    CloseIfOpen(&stdin_pipe[1]);
    CloseIfOpen(&stdout_pipe[0]);
    CloseIfOpen(&stderr_pipe[0]);
    return result;
  }

  if (!SetNonBlocking(stdin_pipe[1]) || !SetNonBlocking(stdout_pipe[0]) ||
      !SetNonBlocking(stderr_pipe[0])) {
    result.launch_failed = true;
    result.stderr_text = std::string("fcntl failed: ") + std::strerror(errno);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    CloseIfOpen(&stdin_pipe[1]);
    CloseIfOpen(&stdout_pipe[0]);
    CloseIfOpen(&stderr_pipe[0]);
    return result;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeout_ms);
  std::size_t input_offset = 0;
  bool stdout_open = true;
  bool stderr_open = true;
  bool stdin_open = true;
  int status = 0;
  bool child_exited = false;
  std::optional<std::chrono::steady_clock::time_point> child_exited_at;

  if (input.empty()) {
    CloseIfOpen(&stdin_pipe[1]);
    stdin_open = false;
  }

  while (stdin_open || stdout_open || stderr_open || !child_exited) {
    if (!child_exited) {
      pid_t wait_rc = waitpid(pid, &status, kWaitNoHang);
      while (wait_rc < 0 && errno == EINTR) {
        wait_rc = waitpid(pid, &status, kWaitNoHang);
      }
      if (wait_rc == pid || (wait_rc < 0 && errno == ECHILD)) {
        child_exited = true;
        child_exited_at = std::chrono::steady_clock::now();
        CloseIfOpen(&stdin_pipe[1]);
        stdin_open = false;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      result.timed_out = true;
      if (!child_exited) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
      }
      CloseIfOpen(&stdin_pipe[1]);
      stdin_open = false;
      break;
    }

    if (child_exited && child_exited_at.has_value()) {
      constexpr auto kPostExitGrace = std::chrono::milliseconds(200);
      if (now - *child_exited_at >= kPostExitGrace) {
        break;
      }
    }

    pollfd fds[3];
    nfds_t nfds = 0;

    if (stdin_open) {
      fds[nfds].fd = stdin_pipe[1];
      fds[nfds].events = POLLOUT;
      fds[nfds].revents = 0;
      ++nfds;
    }
    if (stdout_open) {
      fds[nfds].fd = stdout_pipe[0];
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      ++nfds;
    }
    if (stderr_open) {
      fds[nfds].fd = stderr_pipe[0];
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      ++nfds;
    }

    if (nfds == 0) {
      if (!child_exited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      break;
    }

    int timeout_ms = vinput::time::RemainingTimeoutMs(deadline, 100, now);
    if (child_exited && child_exited_at.has_value()) {
      constexpr auto kPostExitGrace = std::chrono::milliseconds(200);
      timeout_ms =
          vinput::time::RemainingTimeoutMs(*child_exited_at + kPostExitGrace, timeout_ms, now);
    }

    const int poll_rc = poll(fds, nfds, timeout_ms);
    if (poll_rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.launch_failed = true;
      result.stderr_text = std::string("poll failed: ") + std::strerror(errno);
      kill(pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
      break;
    }
    if (poll_rc == 0 && child_exited) {
      break;
    }

    nfds_t index = 0;
    if (stdin_open) {
      if (fds[index].revents & POLLOUT) {
        const std::size_t remaining = input.size() - input_offset;
        if (remaining == 0) {
          CloseIfOpen(&stdin_pipe[1]);
          stdin_open = false;
          ++index;
        } else {
          const auto* bytes = reinterpret_cast<const std::uint8_t*>(input.data()) + input_offset;
          const ssize_t written = write(stdin_pipe[1], bytes, remaining);
          if (written > 0) {
            input_offset += static_cast<std::size_t>(written);
            if (input_offset >= input.size()) {
              CloseIfOpen(&stdin_pipe[1]);
              stdin_open = false;
            }
          } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            CloseIfOpen(&stdin_pipe[1]);
            stdin_open = false;
          }
          ++index;
        }
      } else if (fds[index].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        CloseIfOpen(&stdin_pipe[1]);
        stdin_open = false;
        ++index;
      } else {
        ++index;
      }
    }

    auto drain_fd = [](int fd, std::string* out, bool* open_flag) {
      char buffer[4096];
      while (true) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
          out->append(buffer, static_cast<std::size_t>(n));
          continue;
        }
        if (n == 0) {
          *open_flag = false;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
          if (errno == EINTR) {
            continue;
          }
        }
        break;
      }
    };

    if (stdout_open) {
      if (fds[index].revents & POLLIN) {
        drain_fd(stdout_pipe[0], &result.stdout_text, &stdout_open);
        if (!stdout_open) {
          CloseIfOpen(&stdout_pipe[0]);
        }
      } else if (fds[index].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        drain_fd(stdout_pipe[0], &result.stdout_text, &stdout_open);
        CloseIfOpen(&stdout_pipe[0]);
        stdout_open = false;
      }
      ++index;
    }

    if (stderr_open) {
      if (fds[index].revents & POLLIN) {
        drain_fd(stderr_pipe[0], &result.stderr_text, &stderr_open);
        if (!stderr_open) {
          CloseIfOpen(&stderr_pipe[0]);
        }
      } else if (fds[index].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        drain_fd(stderr_pipe[0], &result.stderr_text, &stderr_open);
        CloseIfOpen(&stderr_pipe[0]);
        stderr_open = false;
      }
    }
  }

  CloseIfOpen(&stdin_pipe[1]);
  CloseIfOpen(&stdout_pipe[0]);
  CloseIfOpen(&stderr_pipe[0]);

  if (result.timed_out) {
    result.exit_code = -1;
    return result;
  }

  if (result.launch_failed) {
    result.exit_code = -1;
    return result;
  }

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }

  return result;
}

bool SpawnForMonitoring(const CommandSpec& spec, const std::filesystem::path& working_dir,
                        SpawnedProcess* process_out, std::string* error) {
  if (process_out) {
    process_out->pid = -1;
    process_out->stderr_fd = -1;
  }
  if (spec.command.empty()) {
    if (error) {
      *error = "Command is empty.";
    }
    return false;
  }

  int stderr_pipe[2] = {-1, -1};
  if (!CreateCloexecPipe(stderr_pipe)) {
    if (error) {
      *error = std::string("pipe failed: ") + std::strerror(errno);
    }
    return false;
  }

  std::vector<std::string> argv_storage;
  std::vector<char*> argv = BuildArgv(spec, &argv_storage);
  std::vector<std::string> env_storage;
  std::vector<char*> envp = BuildEnvp(spec, &env_storage);

  pid_t pid = fork();
  if (pid < 0) {
    CloseIfOpen(&stderr_pipe[0]);
    CloseIfOpen(&stderr_pipe[1]);
    if (error) {
      *error = std::string("fork failed: ") + std::strerror(errno);
    }
    return false;
  }

  if (pid == 0) {
    CloseIfOpen(&stderr_pipe[0]);

    const int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      close(devnull);
    }
    dup2(stderr_pipe[1], STDERR_FILENO);
    CloseIfOpen(&stderr_pipe[1]);

    setsid();

    if (!working_dir.empty()) {
      chdir(working_dir.c_str());
    }

    environ = envp.data();

    execvp(spec.command.c_str(), argv.data());
    _exit(127);
  }

  CloseIfOpen(&stderr_pipe[1]);
  if (!SetNonBlocking(stderr_pipe[0])) {
    CloseIfOpen(&stderr_pipe[0]);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    if (error) {
      *error = std::string("fcntl failed: ") + std::strerror(errno);
    }
    return false;
  }

  if (process_out) {
    process_out->pid = pid;
    process_out->stderr_fd = stderr_pipe[0];
  }
  if (error) {
    error->clear();
  }
  return true;
}

bool RunCommandSilent(const CommandSpec& spec, int* exit_code) {
  if (spec.command.empty()) {
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return false;
  }

  std::vector<std::string> argv_storage;
  std::vector<std::string> envp_storage;
  const auto argv = BuildArgv(spec, &argv_storage);
  const auto envp = BuildEnvp(spec, &envp_storage);

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return false;
  }

  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);

  pid_t pid = -1;
  const int spawn_rc =
      posix_spawnp(&pid, spec.command.c_str(), &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);

  if (spawn_rc != 0 || pid <= 0) {
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return false;
  }

  int status = 0;
  if (spec.timeout_ms <= 0) {
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code != nullptr) {
      *exit_code = rc;
    }
    return rc == 0;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeout_ms);
  while (true) {
    pid_t wait_rc = waitpid(pid, &status, kWaitNoHang);
    while (wait_rc < 0 && errno == EINTR) {
      wait_rc = waitpid(pid, &status, kWaitNoHang);
    }
    if (wait_rc == pid) {
      const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      if (exit_code != nullptr) {
        *exit_code = rc;
      }
      return rc == 0;
    }
    if (wait_rc < 0 && errno == ECHILD) {
      if (exit_code != nullptr) {
        *exit_code = -1;
      }
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      if (exit_code != nullptr) {
        *exit_code = -1;
      }
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

bool RunCommandCaptureStdout(const CommandSpec& spec, std::string* stdout_text, int* exit_code) {
  if (stdout_text != nullptr) {
    stdout_text->clear();
  }
  if (spec.command.empty()) {
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return false;
  }

  int stdout_pipe[2] = {-1, -1};
  if (!CreateCloexecPipe(stdout_pipe)) {
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return false;
  }

  std::vector<std::string> argv_storage;
  std::vector<std::string> envp_storage;
  const auto argv = BuildArgv(spec, &argv_storage);
  const auto envp = BuildEnvp(spec, &envp_storage);

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    CloseIfOpen(&stdout_pipe[0]);
    CloseIfOpen(&stdout_pipe[1]);
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return false;
  }

  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], 1);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);

  pid_t pid = -1;
  const int spawn_rc =
      posix_spawnp(&pid, spec.command.c_str(), &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  CloseIfOpen(&stdout_pipe[1]);

  if (spawn_rc != 0 || pid <= 0) {
    CloseIfOpen(&stdout_pipe[0]);
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return false;
  }

  (void)SetNonBlocking(stdout_pipe[0]);

  const auto deadline = spec.timeout_ms > 0 ? std::chrono::steady_clock::now() +
                                                  std::chrono::milliseconds(spec.timeout_ms)
                                            : std::chrono::steady_clock::time_point::max();

  std::array<char, 4096> buffer{};
  bool stdout_open = true;
  bool child_exited = false;
  int status = 0;

  while (stdout_open || !child_exited) {
    if (!child_exited) {
      pid_t wait_rc = waitpid(pid, &status, kWaitNoHang);
      while (wait_rc < 0 && errno == EINTR) {
        wait_rc = waitpid(pid, &status, kWaitNoHang);
      }
      if (wait_rc == pid || (wait_rc < 0 && errno == ECHILD)) {
        child_exited = true;
      }
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      if (!child_exited) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
      }
      CloseIfOpen(&stdout_pipe[0]);
      if (exit_code != nullptr) {
        *exit_code = -1;
      }
      return false;
    }

    if (!stdout_open) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    pollfd pfd{};
    pfd.fd = stdout_pipe[0];
    pfd.events = POLLIN;

    int timeout_ms = 50;
    if (spec.timeout_ms > 0) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now())
                                 .count();
      timeout_ms = remaining > 0 ? std::min<int>(50, static_cast<int>(remaining)) : 0;
    }

    const int poll_rc = poll(&pfd, 1, timeout_ms);
    if (poll_rc > 0 && (pfd.revents & POLLIN) != 0) {
      while (true) {
        const ssize_t bytes = read(stdout_pipe[0], buffer.data(), buffer.size());
        if (bytes > 0) {
          if (stdout_text != nullptr) {
            stdout_text->append(buffer.data(), static_cast<std::size_t>(bytes));
          }
        } else if (bytes == 0) {
          stdout_open = false;
          CloseIfOpen(&stdout_pipe[0]);
          break;
        } else {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }
          stdout_open = false;
          CloseIfOpen(&stdout_pipe[0]);
          break;
        }
      }
    } else if (poll_rc > 0 && (pfd.revents & (POLLHUP | POLLERR)) != 0) {
      while (true) {
        const ssize_t bytes = read(stdout_pipe[0], buffer.data(), buffer.size());
        if (bytes > 0) {
          if (stdout_text != nullptr) {
            stdout_text->append(buffer.data(), static_cast<std::size_t>(bytes));
          }
        } else {
          break;
        }
      }
      stdout_open = false;
      CloseIfOpen(&stdout_pipe[0]);
    } else if (poll_rc == 0 && child_exited) {
      stdout_open = false;
      CloseIfOpen(&stdout_pipe[0]);
    }
  }

  CloseIfOpen(&stdout_pipe[0]);
  const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (exit_code != nullptr) {
    *exit_code = rc;
  }
  return rc == 0;
}

} // namespace vinput::process
