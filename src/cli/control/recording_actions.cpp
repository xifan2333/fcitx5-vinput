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

int ReportToggleError(Formatter& fmt, const CliContext& ctx, const std::string& msg) {
  if (ctx.json_output) {
    fmt.PrintJson({{"ok", false}, {"error", msg}});
  } else {
    fmt.PrintError(msg);
  }
  return 1;
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
    return ReportToggleError(fmt, ctx, status_err);
  }

  switch (vinput::dbus::StringToStatus(status)) {
  case vinput::dbus::Status::Starting:
  case vinput::dbus::Status::Recording:
    return DoStopRecording(dbus, scene_id, fmt, ctx);
  case vinput::dbus::Status::Stopping:
  case vinput::dbus::Status::Inferring:
  case vinput::dbus::Status::Postprocessing:
    return ReportToggleError(fmt, ctx,
                             ::fmt::sprintf(_("Daemon is busy (status: %s)."), status.c_str()));
  case vinput::dbus::Status::Error:
    return ReportToggleError(fmt, ctx, _("Daemon is in error state."));
  case vinput::dbus::Status::Idle:
    return DoStartRecording(dbus, fmt, ctx);
  }
  return 1;
}
