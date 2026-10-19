#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <sys/types.h>
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

} // namespace vinput::process
