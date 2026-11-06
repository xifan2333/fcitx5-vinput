#include "common/client/client_manager.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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
#include "common/config/core_config_view.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/debug_log.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/process_session.h"
#include "common/utils/process_utils.h"

namespace vinput::client {

namespace fs = std::filesystem;

namespace {

constexpr auto kGracefulStopTimeout = std::chrono::milliseconds(2000);
constexpr auto kForceKillTimeout = std::chrono::milliseconds(1000);

std::string SourceId(std::string_view client_id) {
  return fmt::format("client:{}", client_id);
}

// Reassembles raw stdout chunks into JSONL lines. Chunks arrive on the pump thread
// at arbitrary byte boundaries, so a partial trailing line is held until its '\n'
// arrives; a leftover partial line at session teardown is discarded because it can
// never parse as a complete command.
struct StdoutFramer {
  void Feed(std::span<const std::byte> bytes) {
    std::vector<std::string> lines;
    {
      const std::scoped_lock lock(mutex);
      for (const std::byte b : bytes) {
        const char c = static_cast<char>(b);
        if (c == '\n') {
          lines.push_back(std::move(pending));
          pending.clear();
        } else if (c != '\r') {
          pending.push_back(c);
        }
      }
    }
    if (handler) {
      for (const auto& line : lines) {
        handler(line);
      }
    }
  }

  std::mutex mutex;
  std::string pending;
  CommandHandler handler;
};

struct TrackedSession {
  vinput::process::ProcessSession session;
  vinput::notify::NotificationBridge* bridge = nullptr;
  std::shared_ptr<StdoutFramer> framer;
};

struct TrackedState {
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<TrackedSession>, std::less<>> sessions;
};

TrackedState& GetTrackedState() {
  static TrackedState state;
  return state;
}

// Triggers and receivers share the same spec shape; the protocol layer treats them
// symmetrically, so lookup covers both config domains.
const ClientSpec* FindClient(const CoreConfig& config, std::string_view client_id) {
  const CoreConfigView view(config);
  if (const auto* trigger = view.Trigger(client_id)) {
    return trigger;
  }
  return view.Receiver(client_id);
}

} // namespace

vinput::process::CommandSpec BuildCommandSpec(const ClientSpec& client) {
  vinput::process::CommandSpec spec;
  spec.command = client.command;
  spec.args = client.args;
  spec.env = client.env;
  return spec;
}

std::filesystem::path PidPath(std::string_view client_id) {
  return vinput::path::ClientRuntimeDir() / (std::string(client_id) + ".pid");
}

ClientPidRecord ReadPidRecord(std::string_view client_id) {
  ClientPidRecord record;
  std::ifstream file(PidPath(client_id));
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

bool WritePidFile(std::string_view client_id, pid_t pid, std::string* error) {
  if (pid <= 0) {
    if (error != nullptr) {
      *error = "refusing to persist a non-positive client pid";
    }
    return false;
  }

  const std::uint64_t start_time = vinput::process::ProcessStartTime(pid);
  if (start_time == 0) {
    if (error != nullptr) {
      *error = "failed to read process start time for client pid " + std::to_string(pid);
    }
    return false;
  }

  const std::string content = std::to_string(pid) + ' ' + std::to_string(start_time);
  auto res = vinput::file::AtomicWriteTextFile(PidPath(client_id), content);
  if (!res) {
    if (error != nullptr) {
      *error = res.error();
    }
    return false;
  }
  return true;
}

void RemovePidFile(std::string_view client_id) {
  std::error_code ec;
  fs::remove(PidPath(client_id), ec);
}

pid_t GetPid(std::string_view client_id) {
  {
    auto& state = GetTrackedState();
    const std::scoped_lock lock(state.mutex);
    if (const auto it = state.sessions.find(client_id); it != state.sessions.end()) {
      if (it->second->session.HasExited()) {
        if (it->second->bridge != nullptr) {
          const std::string source_id = SourceId(client_id);
          it->second->bridge->Flush(source_id);
          it->second->bridge->RetireSource(source_id);
        }
        RemovePidFile(client_id);
        state.sessions.erase(it);
        return -1;
      }
      return static_cast<pid_t>(it->second->session.Pid());
    }
  }

  const ClientPidRecord record = ReadPidRecord(client_id);
  const bool alive = vinput::process::ProcessIdentityMatches(record.pid, record.start_time);
  return alive ? record.pid : -1;
}

bool IsRunning(std::string_view client_id) {
  return GetPid(client_id) > 0;
}

bool Start(std::string_view client_id, vinput::notify::NotificationBridge* bridge,
           CommandHandler on_command, std::string* error) {
  auto config = LoadCoreConfig();
  NormalizeCoreConfig(&config);
  const auto* client = FindClient(config, client_id);
  if (client == nullptr) {
    if (error != nullptr) {
      *error = "client not found";
    }
    return false;
  }
  if (client->command.empty()) {
    if (error != nullptr) {
      *error = "client command is not configured";
    }
    return false;
  }
  if (IsRunning(client_id)) {
    if (error != nullptr) {
      *error = "client is already running";
    }
    return false;
  }

  const auto spec = BuildCommandSpec(*client);
  const std::string source_id = SourceId(client_id);
  auto framer = std::make_shared<StdoutFramer>();
  framer->handler = std::move(on_command);

  auto started = vinput::process::ProcessSession::Start(
      spec,
      [bridge, source_id](std::span<const std::byte> bytes) {
        if (bridge != nullptr) {
          bridge->FeedStderr(source_id, bytes);
        }
      },
      [framer](std::span<const std::byte> bytes) { framer->Feed(bytes); });
  if (!started) {
    if (error != nullptr) {
      *error = started.error().empty() ? "failed to start client" : started.error();
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
      *error = fmt::format("client exited immediately (code {})", immediate_exit);
    }
    return false;
  }

  const auto child_pid = static_cast<pid_t>(session.Pid());
  if (!WritePidFile(client_id, child_pid, error)) {
    session.Cancel();
    (void)session.WaitForExit(kGracefulStopTimeout + kForceKillTimeout);
    if (bridge != nullptr) {
      bridge->Flush(source_id);
      bridge->RetireSource(source_id);
    }
    return false;
  }

  auto tracked = std::make_unique<TrackedSession>(TrackedSession{
      .session = std::move(session),
      .bridge = bridge,
      .framer = std::move(framer),
  });

  {
    auto& state = GetTrackedState();
    const std::scoped_lock lock(state.mutex);
    state.sessions.insert_or_assign(std::string(client_id), std::move(tracked));
  }

  VLOG_DEBUG("client started id={}", client_id);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool Stop(std::string_view client_id, std::string* error,
          vinput::notify::NotificationBridge* bridge) {
  std::unique_ptr<TrackedSession> to_stop;
  {
    auto& state = GetTrackedState();
    const std::scoped_lock lock(state.mutex);
    if (const auto it = state.sessions.find(client_id); it != state.sessions.end()) {
      to_stop = std::move(it->second);
      state.sessions.erase(it);
    }
  }

  if (to_stop != nullptr) {
    to_stop->session.Cancel();
    (void)to_stop->session.WaitForExit(kGracefulStopTimeout + kForceKillTimeout);
    auto* b = bridge != nullptr ? bridge : to_stop->bridge;
    if (b != nullptr) {
      const std::string source_id = SourceId(client_id);
      b->Flush(source_id);
      b->RetireSource(source_id);
    }
    RemovePidFile(client_id);
    VLOG_DEBUG("client stopped id={}", client_id);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const ClientPidRecord record = ReadPidRecord(client_id);
  const auto result = vinput::process::TerminateGracefully(record.pid, record.start_time,
                                                           kGracefulStopTimeout, kForceKillTimeout);

  if (bridge != nullptr) {
    const std::string source_id = SourceId(client_id);
    bridge->Flush(source_id);
    bridge->RetireSource(source_id);
  }

  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  const std::string id(client_id);

  switch (result.outcome) {
  case vinput::process::TerminateOutcome::Exited:
    RemovePidFile(client_id);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  case vinput::process::TerminateOutcome::NotFound:
    RemovePidFile(client_id);
    return fail("client is not running: " + id);
  case vinput::process::TerminateOutcome::StillRunning:
    return fail("client did not exit after SIGKILL: " + id);
  case vinput::process::TerminateOutcome::Unconfirmed:
    return fail("could not confirm that client stopped: " + id);
  case vinput::process::TerminateOutcome::Failed:
    return fail((result.failed_signal == SIGKILL ? "failed to force-kill client "
                                                 : "failed to signal client ") +
                id + ": " + result.error);
  }
  return false;
}

void AutoStart(const CoreConfig& config, vinput::notify::NotificationBridge* bridge,
               CommandHandler on_command) {
  const auto autostart = [&](const std::vector<ClientSpec>& clients) {
    for (const auto& client : clients) {
      if (!client.autoStart) {
        continue;
      }
      std::string err;
      if (!Start(client.id, bridge, on_command, &err)) {
        VLOG_ERROR("failed to autostart client '{}': {}", client.id, err);
      } else {
        VLOG_DEBUG("autostarted client '{}'", client.id);
      }
    }
  };
  autostart(config.triggers);
  autostart(config.receivers);
}

void Broadcast(std::string_view line) {
  // PushStdin requires a single owner thread per session, so every broadcast is
  // serialised through the tracked-state lock; the push itself is a bounded queue
  // write and never blocks on the child.
  const std::string frame = std::string(line) + '\n';
  const std::span<const std::byte> bytes = std::as_bytes(std::span(frame));
  auto& state = GetTrackedState();
  const std::scoped_lock lock(state.mutex);
  for (auto& [id, tracked] : state.sessions) {
    if (!tracked->session.PushStdin(bytes)) {
      VLOG_DEBUG("client stdin rejected broadcast id={}", id);
    }
  }
}

void Shutdown() {
  std::vector<std::pair<std::string, std::unique_ptr<TrackedSession>>> to_shutdown;
  {
    auto& state = GetTrackedState();
    const std::scoped_lock lock(state.mutex);
    for (auto& [id, tracked] : state.sessions) {
      to_shutdown.emplace_back(id, std::move(tracked));
    }
    state.sessions.clear();
  }
  for (auto& [id, tracked] : to_shutdown) {
    tracked->session.Cancel();
  }
  for (auto& [id, tracked] : to_shutdown) {
    (void)tracked->session.WaitForExit(kGracefulStopTimeout + kForceKillTimeout);
    if (tracked->bridge != nullptr) {
      const std::string source_id = SourceId(id);
      tracked->bridge->Flush(source_id);
      tracked->bridge->RetireSource(source_id);
    }
    RemovePidFile(id);
  }
}

} // namespace vinput::client
