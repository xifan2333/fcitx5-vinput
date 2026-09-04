#include "common/llm/adapter_manager.h"

#include <cerrno>
#include <csignal>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

#include "common/config/core_config_types.h"
#include "common/utils/path_utils.h"
#include "common/utils/process_utils.h"

namespace vinput::adapter {

namespace fs = std::filesystem;

namespace {

fs::path ExpandConfigPath(const std::string& candidate) {
  if (candidate.empty()) {
    return {};
  }
  fs::path path = vinput::path::ExpandUserPath(candidate);
  if (path.empty()) {
    return {};
  }
  if (path.is_relative()) {
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) {
      return {};
    }
    path = cwd / path;
  }
  return path.lexically_normal();
}

fs::path ResolveScriptPath(const LlmAdapter& adapter) {
  for (const auto& arg : adapter.args) {
    const fs::path path = ExpandConfigPath(arg);
    if (path.empty()) {
      continue;
    }
    std::error_code ec;
    if (fs::exists(path, ec) && !ec && fs::is_regular_file(path, ec) && !ec) {
      return path;
    }
  }

  const fs::path command_path = ExpandConfigPath(adapter.command);
  if (!command_path.empty()) {
    std::error_code ec;
    if (fs::exists(command_path, ec) && !ec && fs::is_regular_file(command_path, ec) && !ec) {
      return command_path;
    }
  }
  return {};
}

pid_t ReadPid(std::string_view adapter_id) {
  std::ifstream file(vinput::path::AdapterRuntimeDir() / (std::string(adapter_id) + ".pid"));
  pid_t pid = -1;
  file >> pid;
  return pid;
}

bool ProcessExists(pid_t pid) {
  if (pid <= 0) {
    return false;
  }
  return kill(pid, 0) == 0 || errno == EPERM;
}

} // namespace

vinput::process::CommandSpec BuildCommandSpec(const LlmAdapter& adapter) {
  vinput::process::CommandSpec spec;
  spec.command = adapter.command;
  spec.args = adapter.args;
  spec.env = adapter.env;
  return spec;
}

std::filesystem::path ResolveWorkingDir(const LlmAdapter& adapter) {
  const fs::path script_path = ResolveScriptPath(adapter);
  if (!script_path.empty()) {
    const fs::path parent = script_path.parent_path();
    if (!parent.empty()) {
      return parent;
    }
  }
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  return ec ? fs::path{} : cwd;
}

std::filesystem::path PidPath(std::string_view adapter_id) {
  return vinput::path::AdapterRuntimeDir() / (std::string(adapter_id) + ".pid");
}

bool WritePidFile(std::string_view adapter_id, pid_t pid, std::string* error) {
  std::error_code ec;
  const fs::path runtime_dir = vinput::path::AdapterRuntimeDir();
  fs::create_directories(runtime_dir, ec);
  if (ec) {
    if (error) {
      *error = "failed to create runtime directory: " + ec.message();
    }
    return false;
  }

  std::ofstream pid_file(PidPath(adapter_id), std::ios::out | std::ios::trunc);
  if (!pid_file.is_open()) {
    if (error) {
      *error = "failed to write pid file: " + std::string(adapter_id);
    }
    return false;
  }
  pid_file << pid;
  if (!pid_file.good()) {
    if (error) {
      *error = "failed to persist pid file: " + std::string(adapter_id);
    }
    return false;
  }
  if (error) {
    error->clear();
  }
  return true;
}

void RemovePidFile(std::string_view adapter_id) {
  std::error_code ec;
  fs::remove(PidPath(adapter_id), ec);
}

pid_t GetPid(std::string_view adapter_id) {
  const pid_t pid = ReadPid(adapter_id);
  return ProcessExists(pid) ? pid : -1;
}

bool IsRunning(std::string_view adapter_id) {
  return GetPid(adapter_id) > 0;
}

bool Stop(std::string_view adapter_id, std::string* error) {
  const pid_t pid = ReadPid(adapter_id);
  if (!ProcessExists(pid)) {
    RemovePidFile(adapter_id);
    if (error) {
      *error = "adapter is not running: " + std::string(adapter_id);
    }
    return false;
  }

  kill(pid, SIGTERM);
  for (int i = 0; i < 20; ++i) {
    if (!ProcessExists(pid)) {
      RemovePidFile(adapter_id);
      if (error) {
        error->clear();
      }
      return true;
    }
    usleep(100000);
  }

  kill(pid, SIGKILL);
  RemovePidFile(adapter_id);
  if (error) {
    error->clear();
  }
  return true;
}

} // namespace vinput::adapter
