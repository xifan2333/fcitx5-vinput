#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>

struct ClientSpec;
struct CoreConfig;
namespace vinput::notify {
class NotificationBridge;
} // namespace vinput::notify
namespace vinput::process {
struct CommandSpec;
} // namespace vinput::process

namespace vinput::client {

// On-disk client identity. A bare pid is not a stable process handle: once the
// client exits the kernel may hand the same pid to an unrelated process, so the
// process start time is persisted alongside it to re-identify the instance.
struct ClientPidRecord {
  pid_t pid = -1;
  // Process start time in clock ticks since boot (/proc/<pid>/stat field 22).
  // Zero means the recorded identity is unusable, e.g. a legacy pid-only file.
  std::uint64_t start_time = 0;
};

// Invoked once per complete stdout line emitted by a client (JSONL framing is
// applied internally). Runs on the session pump thread: handlers must be cheap,
// non-blocking, and must never call back into this manager.
using CommandHandler = std::function<void(std::string_view line)>;

vinput::process::CommandSpec BuildCommandSpec(const ClientSpec& client);
std::filesystem::path PidPath(std::string_view client_id);
ClientPidRecord ReadPidRecord(std::string_view client_id);
bool WritePidFile(std::string_view client_id, pid_t pid, std::string* error);
void RemovePidFile(std::string_view client_id);
pid_t GetPid(std::string_view client_id);
bool IsRunning(std::string_view client_id);

// Starts a trigger or receiver by id (both config domains are searched). Spawns its
// process via ProcessSession, attaches stderr to bridge (if non-null), frames stdout
// into JSONL lines for on_command, validates that the child did not exit immediately
// (250ms probe), persists its PID record, and tracks the session in the host daemon.
bool Start(std::string_view client_id, vinput::notify::NotificationBridge* bridge,
           CommandHandler on_command, std::string* error = nullptr);

// Stops a client by id. If tracked in this host process, cancels its ProcessSession
// and cleans up its bridge source; otherwise terminates the process instance via pid
// record.
bool Stop(std::string_view client_id, std::string* error = nullptr,
          vinput::notify::NotificationBridge* bridge = nullptr);

// Starts all triggers and receivers configured with autoStart in config.
void AutoStart(const CoreConfig& config, vinput::notify::NotificationBridge* bridge,
               CommandHandler on_command);

// Appends '\n' to line and pushes it to the stdin of every tracked client.
// Clients that reject the write (exited or backpressured) are skipped.
void Broadcast(std::string_view line);

// Shuts down all running clients tracked in this process.
void Shutdown();

} // namespace vinput::client
