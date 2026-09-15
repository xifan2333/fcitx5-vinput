#include "common/llm/adapter_manager.h"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

#include "common/config/core_config_types.h"
#include "common/utils/path_utils.h"
#include "common/utils/process_utils.h"

namespace vinput::adapter {

namespace fs = std::filesystem;

namespace {

constexpr int kGracefulStopAttempts = 20;
constexpr int kForceKillAttempts = 10;
constexpr unsigned int kStopPollIntervalUsec = 100000;

// pidfd_open/pidfd_send_signal are Linux 5.3/5.1. Referencing a process through
// a pidfd keeps the kernel object alive, so a signal can never be delivered to a
// recycled pid. The syscall numbers come from the kernel headers, so guard on
// their presence to stay buildable on older toolchains.

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

// Reads field 22 (starttime, clock ticks since boot) of /proc/<pid>/stat.
// Returns 0 when the process is gone or the field cannot be parsed.
std::uint64_t ReadProcessStartTime(pid_t pid) {
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
  // Field 2 is the executable name wrapped in parentheses and may itself
  // contain spaces or parentheses, so resume parsing after the last ')'.
  const std::size_t comm_end = line.rfind(')');
  if (comm_end == std::string::npos || comm_end + 2 >= line.size()) {
    return 0;
  }
  std::istringstream fields(line.substr(comm_end + 2));
  std::string token;
  for (int field = 3; field <= 22; ++field) {
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

// True when the live process at |record.pid| is still the instance the record
// describes. Records without a start time cannot be re-identified, so they are
// deliberately reported as a mismatch rather than trusted.
bool RecordMatchesLiveProcess(const AdapterPidRecord& record) {
  if (record.pid <= 0 || record.start_time == 0) {
    return false;
  }
  return ReadProcessStartTime(record.pid) == record.start_time;
}

// True while the process referenced by |pidfd| has not exited. poll() reports
// readable as soon as the process exits, even before it is reaped.
bool PidFdProcessAlive(int pidfd) {
  pollfd descriptor{};
  descriptor.fd = pidfd;
  descriptor.events = POLLIN;
  const int rc = ::poll(&descriptor, 1, 0);
  if (rc < 0) {
    return false;
  }
  return rc == 0;
}

bool WaitForPidFdExit(int pidfd, int attempts) {
  for (int i = 0; i < attempts; ++i) {
    if (!PidFdProcessAlive(pidfd)) {
      return true;
    }
    usleep(kStopPollIntervalUsec);
  }
  return !PidFdProcessAlive(pidfd);
}

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

  const std::uint64_t start_time = ReadProcessStartTime(pid);
  if (start_time == 0) {
    if (error != nullptr) {
      *error = "failed to read process start time for adapter pid " + std::to_string(pid);
    }
    return false;
  }

  std::error_code ec;
  const fs::path runtime_dir = vinput::path::AdapterRuntimeDir();
  fs::create_directories(runtime_dir, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to create runtime directory: " + ec.message();
    }
    return false;
  }

  std::ofstream pid_file(PidPath(adapter_id), std::ios::out | std::ios::trunc);
  if (!pid_file.is_open()) {
    if (error != nullptr) {
      *error = "failed to write pid file: " + std::string(adapter_id);
    }
    return false;
  }
  pid_file << pid << ' ' << start_time;
  if (!pid_file.good()) {
    if (error != nullptr) {
      *error = "failed to persist pid file: " + std::string(adapter_id);
    }
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void RemovePidFile(std::string_view adapter_id) {
  std::error_code ec;
  fs::remove(PidPath(adapter_id), ec);
}

pid_t GetPid(std::string_view adapter_id) {
  const AdapterPidRecord record = ReadPidRecord(adapter_id);
  return RecordMatchesLiveProcess(record) ? record.pid : -1;
}

bool IsRunning(std::string_view adapter_id) {
  return GetPid(adapter_id) > 0;
}

bool Stop(std::string_view adapter_id, std::string* error) {
  const AdapterPidRecord record = ReadPidRecord(adapter_id);
  if (record.pid <= 0) {
    RemovePidFile(adapter_id);
    if (error != nullptr) {
      *error = "adapter is not running: " + std::string(adapter_id);
    }
    return false;
  }

  if (!RecordMatchesLiveProcess(record)) {
    // No live process carries this identity: either the adapter already exited
    // or the pid was recycled by an unrelated process. Never signal it, only
    // drop the stale state.
    RemovePidFile(adapter_id);
    if (error != nullptr) {
      *error = "adapter is not running, stale pid file removed: " + std::string(adapter_id);
    }
    return false;
  }

  // A pidfd pins this exact process instance, so the signals below cannot be
  // delivered to a recycled pid. Without one there is no safe way to signal by
  // pid, so the adapter is left untouched rather than risk killing an unrelated
  // process.
  const int pidfd = OpenPidFd(record.pid);
  if (pidfd < 0) {
    if (errno == ESRCH) {
      // The adapter exited between the identity check and opening the pidfd.
      RemovePidFile(adapter_id);
      if (error != nullptr) {
        *error = "adapter is not running: " + std::string(adapter_id);
      }
      return false;
    }
    if (error != nullptr) {
      *error = "failed to open pidfd for adapter " + std::string(adapter_id) + ": " +
               std::strerror(errno);
    }
    return false;
  }

  SendSignalViaPidFd(pidfd, SIGTERM);
  if (!WaitForPidFdExit(pidfd, kGracefulStopAttempts)) {
    SendSignalViaPidFd(pidfd, SIGKILL);
    (void)WaitForPidFdExit(pidfd, kForceKillAttempts);
  }
  close(pidfd);

  RemovePidFile(adapter_id);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace vinput::adapter
