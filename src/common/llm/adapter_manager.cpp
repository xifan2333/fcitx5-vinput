#include "common/llm/adapter_manager.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <utility>
#include <vector>

extern "C" {
#include <signal.h>
}

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/debug_log.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/process_session.h"
#include "common/utils/process_utils.h"

namespace vinput::adapter {

namespace fs = std::filesystem;

namespace {

constexpr auto kGracefulStopTimeout = std::chrono::milliseconds(2000);
constexpr auto kForceKillTimeout = std::chrono::milliseconds(1000);

std::string SourceId(std::string_view adapter_id) {
  return std::format("llm:{}", adapter_id);
}

struct TrackedSession {
  vinput::process::ProcessSession session;
  vinput::notify::NotificationBridge* bridge = nullptr;
};

std::mutex g_tracked_mutex;
std::map<std::string, TrackedSession, std::less<>> g_tracked_sessions;

} // namespace

vinput::process::CommandSpec BuildCommandSpec(const LlmAdapter& adapter) {
  vinput::process::CommandSpec spec;
  spec.command = adapter.command;
  spec.args = adapter.args;
  spec.env = adapter.env;
  return spec;
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
  {
    const std::scoped_lock lock(g_tracked_mutex);
    if (const auto it = g_tracked_sessions.find(adapter_id); it != g_tracked_sessions.end()) {
      if (it->second.session.HasExited()) {
        if (it->second.bridge != nullptr) {
          const std::string source_id = SourceId(adapter_id);
          it->second.bridge->Flush(source_id);
          it->second.bridge->RetireSource(source_id);
        }
        RemovePidFile(adapter_id);
        g_tracked_sessions.erase(it);
        return -1;
      }
      return static_cast<pid_t>(it->second.session.Pid());
    }
  }

  const AdapterPidRecord record = ReadPidRecord(adapter_id);
  const bool alive = vinput::process::ProcessIdentityMatches(record.pid, record.start_time);
  return alive ? record.pid : -1;
}

bool IsRunning(std::string_view adapter_id) {
  return GetPid(adapter_id) > 0;
}

bool Start(std::string_view adapter_id, vinput::notify::NotificationBridge* bridge,
           std::string* error) {
  auto config = LoadCoreConfig();
  NormalizeCoreConfig(&config);
  const auto* adapter = ResolveLlmAdapter(config, std::string(adapter_id));
  if (adapter == nullptr) {
    if (error != nullptr) {
      *error = "adapter not found";
    }
    return false;
  }
  if (adapter->command.empty()) {
    if (error != nullptr) {
      *error = "adapter command is not configured";
    }
    return false;
  }
  if (IsRunning(adapter_id)) {
    if (error != nullptr) {
      *error = "adapter is already running";
    }
    return false;
  }

  const auto spec = BuildCommandSpec(*adapter);
  const std::string source_id = SourceId(adapter_id);

  auto started = vinput::process::ProcessSession::Start(
      spec, [bridge, source_id](std::span<const std::byte> bytes) {
        if (bridge != nullptr) {
          bridge->FeedStderr(source_id, bytes);
        }
      });
  if (!started) {
    if (error != nullptr) {
      *error = started.error().empty() ? "failed to start adapter" : started.error();
    }
    return false;
  }

  vinput::process::ProcessSession session = std::move(*started);
  int immediate_exit = -1;
  if (session.WaitForExit(std::chrono::milliseconds(250), &immediate_exit)) {
    if (bridge != nullptr) {
      bridge->Flush(source_id);
      bridge->RetireSource(source_id);
    }
    if (error != nullptr) {
      *error = std::format("adapter exited immediately (code {})", immediate_exit);
    }
    return false;
  }

  const pid_t child_pid = static_cast<pid_t>(session.Pid());
  if (!WritePidFile(adapter_id, child_pid, error)) {
    session.Cancel();
    if (bridge != nullptr) {
      bridge->Flush(source_id);
      bridge->RetireSource(source_id);
    }
    return false;
  }

  {
    const std::scoped_lock lock(g_tracked_mutex);
    g_tracked_sessions.insert_or_assign(
        std::string(adapter_id), TrackedSession{.session = std::move(session), .bridge = bridge});
  }

  VLOG_DEBUG("adapter started id={}", adapter_id);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool Stop(std::string_view adapter_id, std::string* error,
          vinput::notify::NotificationBridge* bridge) {
  std::optional<TrackedSession> to_stop;
  {
    const std::scoped_lock lock(g_tracked_mutex);
    if (const auto it = g_tracked_sessions.find(adapter_id); it != g_tracked_sessions.end()) {
      to_stop.emplace(std::move(it->second));
      g_tracked_sessions.erase(it);
    }
  }

  if (to_stop.has_value()) {
    to_stop->session.Cancel();
    auto* b = bridge != nullptr ? bridge : to_stop->bridge;
    if (b != nullptr) {
      const std::string source_id = SourceId(adapter_id);
      b->Flush(source_id);
      b->RetireSource(source_id);
    }
    RemovePidFile(adapter_id);
    VLOG_DEBUG("adapter stopped id={}", adapter_id);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const AdapterPidRecord record = ReadPidRecord(adapter_id);
  const auto result = vinput::process::TerminateGracefully(record.pid, record.start_time,
                                                           kGracefulStopTimeout, kForceKillTimeout);

  if (bridge != nullptr) {
    const std::string source_id = SourceId(adapter_id);
    bridge->Flush(source_id);
    bridge->RetireSource(source_id);
  }

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
    RemovePidFile(adapter_id);
    return fail("adapter is not running: " + id);
  case vinput::process::TerminateOutcome::StillRunning:
    return fail("adapter did not exit after SIGKILL: " + id);
  case vinput::process::TerminateOutcome::Unconfirmed:
    return fail("could not confirm that adapter stopped: " + id);
  case vinput::process::TerminateOutcome::Failed:
    return fail((result.failed_signal == SIGKILL ? "failed to force-kill adapter "
                                                 : "failed to signal adapter ") +
                id + ": " + result.error);
  }
  return false;
}

void AutoStart(const CoreConfig& config, vinput::notify::NotificationBridge* bridge) {
  for (const auto& adapter : config.llm.adapters) {
    if (!adapter.autoStart) {
      continue;
    }
    std::string err;
    if (!Start(adapter.id, bridge, &err)) {
      fprintf(stderr, "vinput-daemon: failed to autostart adapter '%s': %s\n", adapter.id.c_str(),
              err.c_str());
    } else {
      VLOG_DEBUG("autostarted adapter '{}'", adapter.id);
    }
  }
}

void Shutdown() {
  std::vector<std::pair<std::string, TrackedSession>> to_shutdown;
  {
    const std::scoped_lock lock(g_tracked_mutex);
    for (auto& [id, tracked] : g_tracked_sessions) {
      to_shutdown.emplace_back(id, std::move(tracked));
    }
    g_tracked_sessions.clear();
  }
  for (auto& [id, tracked] : to_shutdown) {
    tracked.session.Cancel();
    if (tracked.bridge != nullptr) {
      const std::string source_id = SourceId(id);
      tracked.bridge->Flush(source_id);
      tracked.bridge->RetireSource(source_id);
    }
    RemovePidFile(id);
  }
}

} // namespace vinput::adapter
