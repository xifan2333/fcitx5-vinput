#include "cli/control/recording_actions.h"

#include <fmt/printf.h>
#include <glaze/json/json_t.hpp>
#include <string>

#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"

#include "cli/runtime/dbus_client.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/cli_helpers.h"
#include "cli/utils/formatter.h"

namespace {

bool EnsureDaemon(vinput::cli::DbusClient& dbus, Formatter& fmt, const CliContext& ctx) {
  std::string err;
  if (!dbus.IsDaemonRunning(&err)) {
    std::string msg = err.empty() ? _("Daemon is not running.") : err;
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", msg}});
    } else {
      fmt.PrintError(msg);
    }
    return false;
  }
  return true;
}

int DoStartRecording(vinput::cli::DbusClient& dbus, Formatter& fmt, const CliContext& ctx) {
  std::string err;
  if (!dbus.StartRecording(&err)) {
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", err}});
    } else {
      fmt.PrintError(err);
    }
    return 1;
  }

  if (ctx.json_output) {
    fmt.PrintJson({{"ok", true}, {"action", "start"}});
  } else {
    fmt.PrintInfo(_("Recording started."));
  }
  return 0;
}

int DoStopRecording(vinput::cli::DbusClient& dbus, const std::string& scene_id, Formatter& fmt,
                    const CliContext& ctx) {
  const std::string resolved = ResolveSceneId(scene_id);

  std::string err;
  if (!dbus.StopRecording(resolved, &err)) {
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", err}});
    } else {
      fmt.PrintError(err);
    }
    return 1;
  }

  if (ctx.json_output) {
    glz::json_t j = {{"ok", true}, {"action", "stop"}};
    if (!resolved.empty()) {
      j["scene"] = resolved;
    }
    fmt.PrintJson(j);
  } else if (resolved.empty()) {
    fmt.PrintInfo(_("Recording stopped."));
  } else {
    fmt.PrintInfo(::fmt::sprintf(_("Recording stopped (scene: %s)."), resolved.c_str()));
  }
  return 0;
}

} // namespace

int RunRecordingControlStart(Formatter& fmt, const CliContext& ctx) {
  vinput::cli::DbusClient dbus;
  if (!EnsureDaemon(dbus, fmt, ctx)) {
    return 1;
  }

  return DoStartRecording(dbus, fmt, ctx);
}

int RunRecordingControlStop(const std::string& scene_id, Formatter& fmt, const CliContext& ctx) {
  vinput::cli::DbusClient dbus;
  if (!EnsureDaemon(dbus, fmt, ctx)) {
    return 1;
  }

  return DoStopRecording(dbus, scene_id, fmt, ctx);
}

int RunRecordingControlToggle(const std::string& scene_id, Formatter& fmt, const CliContext& ctx) {
  vinput::cli::DbusClient dbus;
  if (!EnsureDaemon(dbus, fmt, ctx)) {
    return 1;
  }

  std::string status;
  std::string status_err;
  if (!dbus.GetDaemonStatus(&status, &status_err)) {
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", status_err}});
    } else {
      fmt.PrintError(status_err);
    }
    return 1;
  }

  if (status == vinput::dbus::kStatusRecording) {
    return DoStopRecording(dbus, scene_id, fmt, ctx);
  }

  if (status == vinput::dbus::kStatusInferring || status == vinput::dbus::kStatusPostprocessing) {
    std::string msg = ::fmt::sprintf(_("Daemon is busy (status: %s)."), status.c_str());
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", msg}});
    } else {
      fmt.PrintError(msg);
    }
    return 1;
  }

  if (status == vinput::dbus::kStatusError) {
    std::string msg = _("Daemon is in error state.");
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", msg}});
    } else {
      fmt.PrintError(msg);
    }
    return 1;
  }

  return DoStartRecording(dbus, fmt, ctx);
}
