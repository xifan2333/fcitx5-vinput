#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>

struct CoreConfig;
struct LlmAdapter;
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
std::filesystem::path ResolveWorkingDir(const LlmAdapter& adapter);
std::filesystem::path PidPath(std::string_view adapter_id);
AdapterPidRecord ReadPidRecord(std::string_view adapter_id);
bool WritePidFile(std::string_view adapter_id, pid_t pid, std::string* error);
void RemovePidFile(std::string_view adapter_id);
pid_t GetPid(std::string_view adapter_id);
bool IsRunning(std::string_view adapter_id);
bool Stop(std::string_view adapter_id, std::string* error);

} // namespace vinput::adapter
