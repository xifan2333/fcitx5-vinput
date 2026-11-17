#include "cli/control/daemon_actions.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <string>
#include <unistd.h>

extern "C" {
#include <signal.h>
#include <systemd/sd-bus.h>
}

#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/utils/sandbox.h"
#include "common/utils/string_utils.h"

#include "cli/runtime/dbus_client.h"
#include "cli/runtime/systemd_client.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace {

std::atomic<bool>& PipeInterruptedFlag() {
  static std::atomic<bool> flag{false};
  return flag;
}

void OnPipeSignal(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    PipeInterruptedFlag().store(true, std::memory_order_relaxed);
  }
}

struct PipeStreamContext {
  std::string target_client_id;
  std::atomic<bool> finished{false};
  std::atomic<bool> broken{false};
};

int OnStreamEvent(sd_bus_message* msg, void* userdata, sd_bus_error* /*ret_error*/) {
  auto* pctx = static_cast<PipeStreamContext*>(userdata);
  const char* client_id = nullptr;
  const char* json_line = nullptr;
  if (sd_bus_message_read(msg, "ss", &client_id, &json_line) < 0 || json_line == nullptr) {
    return 0;
  }
  if (client_id == nullptr || pctx->target_client_id != client_id) {
    return 0;
  }
  std::cout << json_line << '\n';
  std::cout.flush();
  if (!std::cout.good()) {
    pctx->broken.store(true, std::memory_order_relaxed);
    return 0;
  }
  if (std::strstr(json_line, R"("status":"idle")") != nullptr) {
    pctx->finished.store(true, std::memory_order_relaxed);
  }
  return 0;
}

void PrintPipeUsageHelp() {
  std::cerr << _("vinput daemon pipe requires audio streamed from standard input.") << "\n\n";
  std::cerr << _("Example usage:") << "\n";
  std::cerr << "  cat audio.wav | vinput d pipe\n";
  std::cerr << "  ffmpeg -i test.mp3 -f s16le -ar 16000 -ac 1 - | vinput d pipe\n";
  std::cerr << "  sox -d -t raw -r 16000 -e signed -b 16 -c 1 - | vinput d pipe | jq -r "
               "'select(.type==\"committed\") | .text'\n";
}

constexpr uint64_t kBusWaitTimeoutUs = 250'000; // 250ms

int ProcessPipeLoop(sd_bus* bus, PipeStreamContext& pctx) {
  while (!pctx.finished.load(std::memory_order_relaxed) &&
         !pctx.broken.load(std::memory_order_relaxed) &&
         !PipeInterruptedFlag().load(std::memory_order_relaxed)) {
    const int r = sd_bus_process(bus, nullptr);
    if (r > 0) {
      continue;
    }
    if (r < 0) {
      return r;
    }
    const int w = sd_bus_wait(bus, kBusWaitTimeoutUs);
    if (w < 0 && w != -EINTR) {
      return w;
    }
  }
  return 0;
}

void PrintSandboxMissingPermissions(Formatter& fmt) {
  if (!vinput::sandbox::IsInSandbox()) {
    return;
  }
  const auto missing = vinput::sandbox::MissingSandboxPermissions();
  if (missing.empty()) {
    return;
  }
  fmt.PrintWarning(_("Missing Flatpak permissions:"));
  for (const auto& perm : missing) {
    fmt.PrintWarning("  - " + perm);
  }
  const std::string override_cmds = vinput::sandbox::FormatOverrideCommands(missing);
  if (override_cmds.empty()) {
    return;
  }
  fmt.PrintInfo(_("To grant permissions, run:"));
  for (const auto line : vinput::str::SplitLines(override_cmds)) {
    if (!line.empty()) {
      fmt.PrintInfo(fmt::format("  {}", line));
    }
  }
}

} // namespace

int RunDaemonControlStatus(Formatter& fmt, const CliContext& ctx) {
  vinput::cli::DbusClient dbus;

  std::string dbus_error;
  const bool running = dbus.IsDaemonRunning(&dbus_error);

  if (!running) {
    if (ctx.json_output) {
      fmt.PrintJson({{"running", false}});
    } else {
      fmt.PrintError(_("Daemon is not running."));
      PrintSandboxMissingPermissions(fmt);
    }
    return 1;
  }

  std::string status;
  std::string get_error;
  if (!dbus.GetDaemonStatus(&status, &get_error)) {
    if (ctx.json_output) {
      fmt.PrintJson({{"running", true}, {"status", nullptr}, {"error", get_error}});
    } else {
      fmt.PrintError(get_error);
    }
    return 1;
  }

  if (ctx.json_output) {
    fmt.PrintJson({{"running", true}, {"status", status}});
  } else {
    char buf[256];
    std::snprintf(buf, sizeof(buf), _("Daemon status: %s"), status.c_str());
    fmt.PrintInfo(buf);
  }

  return 0;
}

int RunDaemonControlStart(Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const auto result = vinput::cli::SystemctlStartWithDiagnostics();
  if (result.ok()) {
    fmt.PrintSuccess(_("Daemon started."));
    return 0;
  }
  fmt.PrintError(result.failure_message);
  return 1;
}

int RunDaemonControlStop(Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const int r = vinput::cli::SystemctlStop();
  if (r == 0) {
    fmt.PrintSuccess(_("Daemon stopped."));
    return 0;
  }
  fmt.PrintError(::fmt::sprintf(_("systemctl stop failed (exit code: %d)"), r));
  return 1;
}

int RunDaemonControlRestart(Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const auto result = vinput::cli::SystemctlRestartWithDiagnostics();
  if (result.ok()) {
    fmt.PrintSuccess(_("Daemon restarted."));
    return 0;
  }
  fmt.PrintError(result.failure_message);
  return 1;
}

int RunDaemonControlEnable(bool force, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const auto result = vinput::cli::SystemctlEnable(force);
  if (result.ok()) {
    fmt.PrintSuccess(_("Daemon enabled to start with system."));
    return 0;
  }
  fmt.PrintError(result.failure_message);
  return 1;
}

int RunDaemonControlDisable(Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const auto result = vinput::cli::SystemctlDisable();
  if (result.ok()) {
    fmt.PrintSuccess(_("Daemon disabled from starting with system."));
    return 0;
  }
  fmt.PrintError(result.failure_message);
  return 1;
}

int RunDaemonControlLogs(bool follow, int lines, const std::string& level, Formatter& fmt,
                         const CliContext& ctx) {
  (void)fmt;
  (void)ctx;
  return vinput::cli::JournalctlLogs(follow, lines, level);
}

int RunDaemonControlPipe(const std::string& scene_id, bool ime, Formatter& /*fmt*/,
                         const CliContext& /*ctx*/) {
  if (isatty(STDIN_FILENO) != 0) {
    PrintPipeUsageHelp();
    return 1;
  }

  vinput::cli::DbusClient dbus;
  std::string dbus_error;
  if (!dbus.IsDaemonRunning(&dbus_error)) {
    std::cerr << _("Daemon is not running.") << '\n';
    return 1;
  }

  PipeStreamContext pctx;
  sd_bus_slot* slot = nullptr;
  const int match_rc = sd_bus_match_signal(dbus.GetBus(), &slot, vinput::dbus::kBusName,
                                           vinput::dbus::kObjectPath, vinput::dbus::kInterface,
                                           vinput::dbus::kSignalStreamEvent, OnStreamEvent, &pctx);
  if (match_rc < 0 || slot == nullptr) {
    std::cerr << _("Failed to subscribe to daemon stream events.") << '\n';
    return 1;
  }

  std::string start_error;
  if (!dbus.TranscribeStream(STDIN_FILENO, scene_id, ime, &pctx.target_client_id, &start_error)) {
    std::cerr << ::fmt::sprintf(_("Failed to start audio transcription stream: %s"), start_error)
              << '\n';
    sd_bus_slot_unref(slot);
    return 1;
  }

  PipeInterruptedFlag().store(false, std::memory_order_relaxed);
  const auto prev_int = signal(SIGINT, OnPipeSignal);
  const auto prev_term = signal(SIGTERM, OnPipeSignal);
  const auto prev_pipe = signal(SIGPIPE, SIG_IGN);

  const int loop_rc = ProcessPipeLoop(dbus.GetBus(), pctx);

  signal(SIGINT, prev_int);
  signal(SIGTERM, prev_term);
  signal(SIGPIPE, prev_pipe);
  sd_bus_slot_unref(slot);

  if (!pctx.finished.load(std::memory_order_relaxed) && !pctx.target_client_id.empty()) {
    dbus.StopClient(pctx.target_client_id);
  }

  if (loop_rc < 0) {
    std::cerr << ::fmt::sprintf(_("D-Bus event loop error: %s"), strerror(-loop_rc)) << '\n';
    return 1;
  }
  if (PipeInterruptedFlag().load(std::memory_order_relaxed)) {
    return 130;
  }
  if (pctx.broken.load(std::memory_order_relaxed)) {
    return 1;
  }

  return 0;
}
