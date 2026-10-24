#include "common/llm/adapter_manager.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>

extern "C" {
#include <signal.h>
}

#include "common/config/core_config_types.h"
#include "common/utils/file_utils.h"
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

  fs::path command_path = ExpandConfigPath(adapter.command);
  if (!command_path.empty()) {
    std::error_code ec;
    if (fs::exists(command_path, ec) && !ec && fs::is_regular_file(command_path, ec) && !ec) {
      return command_path;
    }
  }
  return {};
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

AdapterPidRecord ReadPidRecord(std::string_view adapter_id) {
  AdapterPidRecord record;
  std::ifstream file(PidPath(adapter_id));
  if (!file.is_open()) {
    return record;
  }
  file >> record.pid;
  // Legacy pid-only files carry no identity, leaving start_time at zero so the
  // record is reported as unverifiable instead of being trusted blindly.
  file >> record.start_time;
  if (record.pid <= 0) {
    record.pid = -1;
    record.start_time = 0;
  }
  return record;
}

bool WritePidFile(std::string_view adapter_id, pid_t pid, std::string* error) {
  if (pid <= 0) {
    if (error != nullptr) {
      *error = "refusing to persist a non-positive adapter pid";
    }
    return false;
  }

  const std::uint64_t start_time = vinput::process::ProcessStartTime(pid);
  if (start_time == 0) {
    if (error != nullptr) {
      *error = "failed to read process start time for adapter pid " + std::to_string(pid);
    }
    return false;
  }

  const std::string content = std::to_string(pid) + ' ' + std::to_string(start_time);
  auto res = vinput::file::AtomicWriteTextFile(PidPath(adapter_id), content);
  if (!res) {
    if (error != nullptr) {
      *error = res.error();
    }
    return false;
  }
  return true;
}

void RemovePidFile(std::string_view adapter_id) {
  std::error_code ec;
  fs::remove(PidPath(adapter_id), ec);
}

pid_t GetPid(std::string_view adapter_id) {
  const AdapterPidRecord record = ReadPidRecord(adapter_id);
  const bool alive = vinput::process::ProcessIdentityMatches(record.pid, record.start_time);
  return alive ? record.pid : -1;
}

bool IsRunning(std::string_view adapter_id) {
  return GetPid(adapter_id) > 0;
}

namespace {

// An adapter that ignores SIGTERM is given two seconds to wind down before the
// forced kill, and one more second to disappear afterwards.
constexpr auto kGracefulStopTimeout = std::chrono::milliseconds(2000);
constexpr auto kForceKillTimeout = std::chrono::milliseconds(1000);

} // namespace

bool Stop(std::string_view adapter_id, std::string* error) {
  const AdapterPidRecord record = ReadPidRecord(adapter_id);
  const auto result = vinput::process::TerminateGracefully(record.pid, record.start_time,
                                                           kGracefulStopTimeout, kForceKillTimeout);

  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  const std::string id(adapter_id);

  switch (result.outcome) {
  case vinput::process::TerminateOutcome::Exited:
    RemovePidFile(adapter_id);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  case vinput::process::TerminateOutcome::NotFound:
    // Either the adapter already exited or its pid was recycled: drop the stale
    // record, nothing was signalled.
    RemovePidFile(adapter_id);
    return fail("adapter is not running: " + id);
  case vinput::process::TerminateOutcome::StillRunning:
    // Keep the record so the adapter can still be located and retried.
    return fail("adapter did not exit after SIGKILL: " + id);
  case vinput::process::TerminateOutcome::Unconfirmed:
    return fail("could not confirm that adapter stopped: " + id);
  case vinput::process::TerminateOutcome::Failed:
    // The escalation phase belongs in the diagnosis: a rejected SIGTERM and a
    // rejected SIGKILL point at very different conditions.
    return fail((result.failed_signal == SIGKILL ? "failed to force-kill adapter "
                                                 : "failed to signal adapter ") +
                id + ": " + result.error);
  }
  return false;
}

} // namespace vinput::adapter
