#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>

struct CoreConfig;
struct LlmAdapter;
namespace vinput::notify {
class NotificationBridge;
} // namespace vinput::notify
namespace vinput::process {
struct CommandSpec;
}

namespace vinput::adapter {

// On-disk adapter identity. A bare pid is not a stable process handle: once the
// adapter exits the kernel may hand the same pid to an unrelated process, so the
// process start time is persisted alongside it to re-identify the instance.
struct AdapterPidRecord {
  pid_t pid = -1;
  // Process start time in clock ticks since boot (/proc/<pid>/stat field 22).
  // Zero means the recorded identity is unusable, e.g. a legacy pid-only file.
  std::uint64_t start_time = 0;
};

vinput::process::CommandSpec BuildCommandSpec(const LlmAdapter& adapter);
std::filesystem::path PidPath(std::string_view adapter_id);
AdapterPidRecord ReadPidRecord(std::string_view adapter_id);
bool WritePidFile(std::string_view adapter_id, pid_t pid, std::string* error);
void RemovePidFile(std::string_view adapter_id);
pid_t GetPid(std::string_view adapter_id);
bool IsRunning(std::string_view adapter_id);

// Starts an adapter by id. Spawns its process via ProcessSession, attaches its stderr to bridge
// (if non-null), validates that the child did not exit immediately (250ms probe), persists its PID
// record, and tracks the session in the host daemon.
bool Start(std::string_view adapter_id, vinput::notify::NotificationBridge* bridge,
           std::string* error = nullptr);

// Stops an adapter by id. If tracked in this host process, cancels its ProcessSession and cleans
// up its bridge source; otherwise terminates the process instance via pid record.
bool Stop(std::string_view adapter_id, std::string* error = nullptr,
          vinput::notify::NotificationBridge* bridge = nullptr);

// Starts all adapters configured with autoStart in config.
void AutoStart(const CoreConfig& config, vinput::notify::NotificationBridge* bridge);

// Shuts down all running adapters tracked in this process.
void Shutdown();

} // namespace vinput::adapter
