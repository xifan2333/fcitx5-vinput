#include "common/llm/adapter_manager.h"

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

extern "C" {
#include <signal.h>
}

#include "common/config/core_config_types.h"
#include "common/utils/path_utils.h"
#include "common/utils/process_utils.h"

namespace vinput::adapter {

namespace fs = std::filesystem;

namespace {

constexpr int kGracefulStopAttempts = 20;
constexpr int kForceKillAttempts = 10;
constexpr int kProbeAttempts = 3;
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

// How the pinned process was observed when the descriptor was probed.
enum class ProcessState : std::uint8_t { Running, Exited, Unknown };

// poll() reports readable as soon as the process exits, even before it is reaped.
// An interrupted or failed probe says nothing about the process, so it is reported
// as Unknown rather than being taken for either outcome.
ProcessState ProbePinnedProcess(int pidfd) {
  pollfd descriptor{};
  descriptor.fd = pidfd;
  descriptor.events = POLLIN;
  for (int attempt = 0; attempt < kProbeAttempts; ++attempt) {
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

// Outcome of waiting for the pinned process to exit.
enum class ExitWait : std::uint8_t { Exited, StillRunning, Inconclusive };

// Waits for the pinned process to exit. Only a readable descriptor confirms exit,
// and an interrupted or failed probe is never folded into one of the other
// outcomes, so it can also never be mistaken for a confirmed exit.
ExitWait WaitForPidFdExit(int pidfd, int attempts) {
  bool observed_running = false;
  for (int i = 0; i < attempts; ++i) {
    const ProcessState state = ProbePinnedProcess(pidfd);
    if (state == ProcessState::Exited) {
      return ExitWait::Exited;
    }
    if (state == ProcessState::Running) {
      observed_running = true;
    }
    usleep(kStopPollIntervalUsec);
  }
  return observed_running ? ExitWait::StillRunning : ExitWait::Inconclusive;
}

// Delivers |signal_number| to the pinned process, retrying when interrupted.
// Returns false when the signal could not be delivered; errno then explains why.
bool SignalPinnedProcess(int pidfd, int signal_number) {
  while (true) {
    if (SendSignalViaPidFd(pidfd, signal_number)) {
      return true;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

// Result of delivering a signal to a process identified only by its pid.
enum class SignalOutcome : std::uint8_t { Delivered, ProcessGone, Failed };

// Sends |signal_number| to |pid| without a descriptor, for the cases where
// pidfd_open is unavailable. The kill syscall is preferred over tgkill because it
// keeps the signal process-directed: tgkill(pid, pid, ...) would only reach the
// thread-group leader, so an adapter that handles termination on another thread
// could swallow the request. Returns false and sets errno when the signal could
// not be delivered.
bool SendSignalByPid(pid_t pid, int signal_number) {
#if defined(SYS_kill)
  return ::syscall(SYS_kill, pid, signal_number) == 0;
#else
  (void)pid;
  (void)signal_number;
  errno = ENOSYS;
  return false;
#endif
}

// Re-confirms the recorded identity and then signals the process. The check runs
// immediately before the signal, so a pid recycled earlier cannot be reached.
SignalOutcome SignalVerifiedProcess(const AdapterPidRecord& record, int signal_number) {
  if (!RecordMatchesLiveProcess(record)) {
    return SignalOutcome::ProcessGone;
  }
  if (SendSignalByPid(record.pid, signal_number)) {
    return SignalOutcome::Delivered;
  }
  return errno == ESRCH ? SignalOutcome::ProcessGone : SignalOutcome::Failed;
}

// True once the recorded process is no longer identifiable, which covers both a
// clean exit and a pid recycled by an unrelated process.
bool WaitForProcessExit(const AdapterPidRecord& record, int attempts) {
  for (int i = 0; i < attempts; ++i) {
    if (!RecordMatchesLiveProcess(record)) {
      return true;
    }
    usleep(kStopPollIntervalUsec);
  }
  return !RecordMatchesLiveProcess(record);
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

namespace {

// Fallback used when a pidfd cannot be obtained, for example on a kernel without
// pidfd_open or when file descriptors are exhausted. Because there is no stable
// handle, the recorded identity is re-checked before every signal, which leaves
// only the gap between that check and the signal as a reuse window.
bool StopByPid(const AdapterPidRecord& record, std::string_view adapter_id, std::string* error) {
  const SignalOutcome terminate = SignalVerifiedProcess(record, SIGTERM);
  if (terminate == SignalOutcome::Failed) {
    if (error != nullptr) {
      *error = "failed to signal adapter " + std::string(adapter_id) + ": " + std::strerror(errno);
    }
    return false;
  }
  if (terminate == SignalOutcome::ProcessGone) {
    RemovePidFile(adapter_id);
    if (error != nullptr) {
      *error = "adapter is not running: " + std::string(adapter_id);
    }
    return false;
  }
  if (WaitForProcessExit(record, kGracefulStopAttempts)) {
    RemovePidFile(adapter_id);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const SignalOutcome forced = SignalVerifiedProcess(record, SIGKILL);
  if (forced == SignalOutcome::Failed) {
    // Keep the record so the surviving adapter can still be located and retried.
    if (error != nullptr) {
      *error =
          "failed to force-kill adapter " + std::string(adapter_id) + ": " + std::strerror(errno);
    }
    return false;
  }
  if (forced == SignalOutcome::ProcessGone || WaitForProcessExit(record, kForceKillAttempts)) {
    RemovePidFile(adapter_id);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  // Keep the record so the surviving adapter can still be located and retried.
  if (error != nullptr) {
    *error = "adapter did not exit after SIGKILL: " + std::string(adapter_id);
  }
  return false;
}

} // namespace

bool Stop(std::string_view adapter_id, std::string* error) {
  const AdapterPidRecord record = ReadPidRecord(adapter_id);
  if (record.pid <= 0) {
    RemovePidFile(adapter_id);
    if (error != nullptr) {
      *error = "adapter is not running: " + std::string(adapter_id);
    }
    return false;
  }

  // Pin the process before validating its identity. Holding the descriptor keeps
  // this exact instance referenced, so a pid recycled between the check and the
  // signals below can never be reached: verification and delivery act on the same
  // kernel object.
  const int pidfd = OpenPidFd(record.pid);
  if (pidfd < 0) {
    if (errno == ESRCH) {
      RemovePidFile(adapter_id);
      if (error != nullptr) {
        *error = "adapter is not running: " + std::string(adapter_id);
      }
      return false;
    }
    // No descriptor is available, so fall back to signalling by pid. Every signal
    // is preceded by a fresh identity check, which still narrows the pid-reuse
    // window to the gap between that check and the signal itself.
    return StopByPid(record, adapter_id, error);
  }

  if (!RecordMatchesLiveProcess(record)) {
    // The pinned process is not the adapter: either the adapter already exited or
    // the pid was recycled. Signals sent through the descriptor could only reach
    // the pinned instance, so drop the stale state and signal nothing.
    close(pidfd);
    RemovePidFile(adapter_id);
    if (error != nullptr) {
      *error = "adapter is not running, stale pid file removed: " + std::string(adapter_id);
    }
    return false;
  }

  if (!SignalPinnedProcess(pidfd, SIGTERM)) {
    const int signal_errno = errno;
    close(pidfd);
    if (signal_errno == ESRCH) {
      // The adapter exited before the signal could be delivered.
      RemovePidFile(adapter_id);
      if (error != nullptr) {
        *error = "adapter is not running: " + std::string(adapter_id);
      }
      return false;
    }
    // Keep the record so the surviving adapter can still be located and retried.
    if (error != nullptr) {
      *error = "failed to signal adapter " + std::string(adapter_id) + ": " +
               std::strerror(signal_errno);
    }
    return false;
  }

  ExitWait wait = WaitForPidFdExit(pidfd, kGracefulStopAttempts);
  if (wait != ExitWait::Exited) {
    if (!SignalPinnedProcess(pidfd, SIGKILL)) {
      const int signal_errno = errno;
      if (signal_errno != ESRCH) {
        close(pidfd);
        if (error != nullptr) {
          *error = "failed to force-kill adapter " + std::string(adapter_id) + ": " +
                   std::strerror(signal_errno);
        }
        return false;
      }
    }
    wait = WaitForPidFdExit(pidfd, kForceKillAttempts);
  }
  close(pidfd);

  if (wait != ExitWait::Exited) {
    // Keep the record so the adapter can still be located and retried, and report
    // honestly whether it was observed alive or merely could not be confirmed.
    if (error != nullptr) {
      *error = wait == ExitWait::StillRunning
                   ? "adapter did not exit after SIGKILL: " + std::string(adapter_id)
                   : "could not confirm that adapter stopped: " + std::string(adapter_id);
    }
    return false;
  }

  RemovePidFile(adapter_id);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace vinput::adapter
