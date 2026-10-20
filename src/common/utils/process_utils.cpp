#include "common/utils/process_utils.h"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <exception>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <stdio.h>
#include <string>
#include <subprocess.hpp>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/utils/interpolation.h"

extern char** environ;

namespace vinput::process {

namespace {

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
  for (char** env = environ; env && *env; ++env) {
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

    std::vector<char> in_buf(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      in_buf[i] = static_cast<char>(input[i]);
    }

    if (spec.timeout_ms > 0) {
      std::mutex comm_mutex;
      std::condition_variable comm_cv;
      bool done = false;
      subprocess::Buffer out_buf;
      subprocess::Buffer err_buf;

      std::thread comm_thread([&] {
        try {
          auto res = proc.communicate(in_buf);
          const std::scoped_lock lock(comm_mutex);
          out_buf = std::move(res.first);
          err_buf = std::move(res.second);
          done = true;
        } catch (const std::exception&) {
          const std::scoped_lock lock(comm_mutex);
          done = true;
        }
        comm_cv.notify_one();
      });

      std::unique_lock lock(comm_mutex);
      if (!comm_cv.wait_for(lock, std::chrono::milliseconds(spec.timeout_ms),
                            [&] { return done; })) {
        result.timed_out = true;
        proc.kill(SIGKILL);
        if (proc.output() != nullptr) {
          ::close(fileno(proc.output()));
        }
        if (proc.error() != nullptr) {
          ::close(fileno(proc.error()));
        }
        lock.unlock();
        comm_thread.join();
        return result;
      }
      lock.unlock();
      comm_thread.join();

      result.exit_code = proc.retcode();
      result.stdout_text.assign(out_buf.buf.data(), out_buf.length);
      result.stderr_text.assign(err_buf.buf.data(), err_buf.length);
      return result;
    }

    auto [out_buf, err_buf] = proc.communicate(in_buf);
    result.exit_code = proc.retcode();
    result.stdout_text.assign(out_buf.buf.data(), out_buf.length);
    result.stderr_text.assign(err_buf.buf.data(), err_buf.length);
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

bool SpawnForMonitoring(const CommandSpec& spec, const std::filesystem::path& working_dir,
                        SpawnedProcess* process_out, std::string* error) {
  if (process_out != nullptr) {
    process_out->pid = -1;
    process_out->stderr_fd = -1;
  }
  if (spec.command.empty()) {
    if (error != nullptr) {
      *error = "Command is empty.";
    }
    return false;
  }

  const auto cmd = BuildCmd(spec);
  const auto env_map = BuildEnvMap(spec);

  try {
    auto proc = subprocess::Popen(
        cmd, subprocess::input{"/dev/null"}, subprocess::output{"/dev/null"},
        subprocess::error{subprocess::PIPE}, subprocess::cwd{working_dir.string()},
        subprocess::environment{env_map}, subprocess::session_leader{true});

    if (process_out != nullptr) {
      process_out->pid = proc.pid();
      FILE* err_fp = proc.error();
      if (err_fp != nullptr) {
        process_out->stderr_fd = dup(fileno(err_fp));
      }
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception& e) {
    if (error != nullptr) {
      *error = e.what();
    }
    return false;
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

  const auto result = RunCommandWithInput(spec, {});
  if (stdout_text != nullptr) {
    *stdout_text = result.stdout_text;
  }
  if (exit_code != nullptr) {
    *exit_code = result.exit_code;
  }
  return !result.launch_failed && !result.timed_out && result.exit_code == 0;
}

} // namespace vinput::process
