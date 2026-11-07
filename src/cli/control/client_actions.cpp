#include "cli/control/client_actions.h"

#include <csignal>
#include <cstdint>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <glaze/json/json_t.hpp>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <systemd/sd-bus.h>
}

#include "common/asr/recognition_result.h"
#include "common/client/client_manager.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"

#include "cli/runtime/dbus_client.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli::control {

namespace {

volatile std::sig_atomic_t g_listen_interrupted = 0;

void OnListenSignal(int /*sig*/) {
  g_listen_interrupted = 1;
}

const char* KindLabel(ClientKind kind) {
  return kind == ClientKind::Trigger ? _("trigger") : _("receiver");
}

const std::vector<ClientSpec>& GetClientList(const CoreConfig& config, ClientKind kind) {
  return kind == ClientKind::Trigger ? config.triggers : config.receivers;
}

} // namespace

int RunClientList(ClientKind kind, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const auto& clients = GetClientList(config, kind);

  if (ctx.json_output) {
    glz::json_t::array_t arr;
    arr.reserve(clients.size());
    for (const auto& client : clients) {
      const bool running = vinput::client::IsRunning(client.id);
      const auto pid = vinput::client::GetPid(client.id);
      glz::json_t entry = {
          {"id", client.id},
          {"running", running},
          {"pid", pid > 0 ? glz::json_t(pid) : glz::json_t(nullptr)},
          {"auto_start", client.autoStart},
          {"command", client.command},
      };
      arr.emplace_back(std::move(entry));
    }
    fmt.PrintJson(glz::json_t(std::move(arr)));
    return 0;
  }

  if (clients.empty()) {
    fmt.PrintInfo(::fmt::sprintf(_("No %ss configured."), KindLabel(kind)));
    return 0;
  }

  const std::vector<std::string> headers = {_("ID"), _("STATUS"), _("PID"), _("AUTOSTART"),
                                            _("COMMAND")};
  std::vector<std::vector<std::string>> rows;
  rows.reserve(clients.size());
  for (const auto& client : clients) {
    const bool running = vinput::client::IsRunning(client.id);
    const auto pid = vinput::client::GetPid(client.id);
    rows.push_back({
        client.id,
        running ? _("running") : _("stopped"),
        pid > 0 ? std::to_string(pid) : "-",
        client.autoStart ? _("enabled") : _("disabled"),
        client.command,
    });
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunClientStart(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  vinput::cli::DbusClient dbus;
  std::string error;
  if (!dbus.StartClient(id, &error)) {
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", error}});
    } else {
      fmt.PrintError(error);
    }
    return 1;
  }

  if (ctx.json_output) {
    fmt.PrintJson({{"ok", true}, {"id", id}, {"action", "start"}});
  } else {
    fmt.PrintSuccess(::fmt::sprintf(_("Client '%s' started."), id));
  }
  return 0;
}

int RunClientStop(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  vinput::cli::DbusClient dbus;
  std::string error;
  if (!dbus.StopClient(id, &error)) {
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", error}});
    } else {
      fmt.PrintError(error);
    }
    return 1;
  }

  if (ctx.json_output) {
    fmt.PrintJson({{"ok", true}, {"id", id}, {"action", "stop"}});
  } else {
    fmt.PrintSuccess(::fmt::sprintf(_("Client '%s' stopped."), id));
  }
  return 0;
}

struct ListenContext {
  std::string type_filter;
  bool json_output = false;
};

static int OnRecognitionPartial(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
  auto* lctx = static_cast<ListenContext*>(userdata);
  if (lctx->type_filter != "all" && lctx->type_filter != "partial") {
    return 0;
  }
  const char* text = "";
  if (sd_bus_message_read(m, "s", &text) < 0 || text == nullptr) {
    return 0;
  }
  if (lctx->json_output) {
    std::cout << fmt::format(R"({{"type":"partial","text":"{}"}})", text) << '\n';
  } else {
    std::cout << text << '\n';
  }
  std::cout.flush();
  return 0;
}

static int OnRecognitionResult(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
  auto* lctx = static_cast<ListenContext*>(userdata);
  if (lctx->type_filter != "all" && lctx->type_filter != "committed") {
    return 0;
  }
  const char* raw_json = "";
  if (sd_bus_message_read(m, "s", &raw_json) < 0 || raw_json == nullptr) {
    return 0;
  }
  const auto payload = vinput::result::Parse(raw_json);
  if (payload.commitText.empty()) {
    return 0;
  }
  if (lctx->json_output) {
    std::cout << fmt::format(R"({{"type":"committed","text":"{}"}})", payload.commitText) << '\n';
  } else {
    std::cout << payload.commitText << '\n';
  }
  std::cout.flush();
  return 0;
}

static int OnStatusChanged(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
  auto* lctx = static_cast<ListenContext*>(userdata);
  if (lctx->type_filter != "all" && lctx->type_filter != "status") {
    return 0;
  }
  const char* status = "";
  if (sd_bus_message_read(m, "s", &status) < 0 || status == nullptr) {
    return 0;
  }
  if (lctx->json_output) {
    std::cout << fmt::format(R"({{"type":"status","status":"{}"}})", status) << '\n';
  } else {
    std::cout << status << '\n';
  }
  std::cout.flush();
  return 0;
}

int RunListen(std::string_view type_filter, Formatter& /*fmt*/, const CliContext& ctx) {
  ListenContext lctx;
  lctx.type_filter = type_filter.empty() ? "committed" : std::string(type_filter);
  lctx.json_output = ctx.json_output;

  sd_bus* bus = nullptr;
  if (sd_bus_default_user(&bus) < 0 || bus == nullptr) {
    std::cerr << _("Failed to connect to user D-Bus.") << '\n';
    return 1;
  }

  sd_bus_slot* slot_partial = nullptr;
  sd_bus_slot* slot_result = nullptr;
  sd_bus_slot* slot_status = nullptr;

  (void)sd_bus_match_signal(bus, &slot_partial, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                            vinput::dbus::kInterface, vinput::dbus::kSignalRecognitionPartial,
                            OnRecognitionPartial, &lctx);

  (void)sd_bus_match_signal(bus, &slot_result, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                            vinput::dbus::kInterface, vinput::dbus::kSignalRecognitionResult,
                            OnRecognitionResult, &lctx);

  (void)sd_bus_match_signal(bus, &slot_status, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                            vinput::dbus::kInterface, vinput::dbus::kSignalStatusChanged,
                            OnStatusChanged, &lctx);

  g_listen_interrupted = 0;
  const auto prev_int = std::signal(SIGINT, OnListenSignal);
  const auto prev_term = std::signal(SIGTERM, OnListenSignal);

  while (g_listen_interrupted == 0) {
    const int r = sd_bus_process(bus, nullptr);
    if (r > 0) {
      continue;
    }
    if (r < 0) {
      break;
    }
    const int w = sd_bus_wait(bus, 250000); // 250ms
    if (w < 0 && w != -EINTR) {
      break;
    }
  }

  std::signal(SIGINT, prev_int);
  std::signal(SIGTERM, prev_term);

  if (slot_partial != nullptr) {
    sd_bus_slot_unref(slot_partial);
  }
  if (slot_result != nullptr) {
    sd_bus_slot_unref(slot_result);
  }
  if (slot_status != nullptr) {
    sd_bus_slot_unref(slot_status);
  }
  sd_bus_flush_close_unref(bus);
  return 0;
}

} // namespace vinput::cli::control
