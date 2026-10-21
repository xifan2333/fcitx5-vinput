#include "cli/control/recording_actions.h"

#include <glaze/json/json_t.hpp>
#include <string>

#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/utils/string_utils.h"

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

} // namespace

int RunRecordingControlStart(Formatter& fmt, const CliContext& ctx) {
  vinput::cli::DbusClient dbus;
  if (!EnsureDaemon(dbus, fmt, ctx)) {
    return 1;
  }

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

int RunRecordingControlStop(const std::string& scene_id, Formatter& fmt, const CliContext& ctx) {
  vinput::cli::DbusClient dbus;
  if (!EnsureDaemon(dbus, fmt, ctx)) {
    return 1;
  }

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
    fmt.PrintInfo(vinput::str::FmtStr(_("Recording stopped (scene: %s)."), resolved.c_str()));
  }
  return 0;
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
    const std::string resolved = ResolveSceneId(scene_id);

    std::string stop_err;
    if (!dbus.StopRecording(resolved, &stop_err)) {
      if (ctx.json_output) {
        fmt.PrintJson({{"ok", false}, {"error", stop_err}});
      } else {
        fmt.PrintError(stop_err);
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
      fmt.PrintInfo(vinput::str::FmtStr(_("Recording stopped (scene: %s)."), resolved.c_str()));
    }
    return 0;
  }

  if (status == vinput::dbus::kStatusInferring || status == vinput::dbus::kStatusPostprocessing) {
    std::string msg = vinput::str::FmtStr(_("Daemon is busy (status: %s)."), status.c_str());
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
