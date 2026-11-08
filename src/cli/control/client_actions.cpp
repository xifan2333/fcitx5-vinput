#include "cli/control/client_actions.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fmt/printf.h>
#include <glaze/json/json_t.hpp>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <systemd/sd-bus.h>
}

#include "common/asr/recognition_result.h"
#include "common/client/client_event.h"
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

std::atomic<bool>& ListenInterrupted() {
  static std::atomic<bool> interrupted{false};
  return interrupted;
}

void OnListenSignal(int /*sig*/) {
  ListenInterrupted().store(true, std::memory_order_relaxed);
}

const char* KindLabel(ClientKind kind) {
  return kind == ClientKind::Trigger ? _("trigger") : _("receiver");
}

const std::vector<ClientSpec>& GetClientList(const CoreConfig& config, ClientKind kind) {
  return kind == ClientKind::Trigger ? config.triggers : config.receivers;
}

bool HasClientWithId(const CoreConfig& config, ClientKind kind, std::string_view id) {
  const auto& list = GetClientList(config, kind);
  return std::ranges::any_of(list, [&](const auto& client) { return client.id == id; });
}

void PrintClientListJson(Formatter& fmt, const std::vector<ClientSpec>& clients,
                         const vinput::cli::ClientStatusMap& status_map) {
  glz::json_t::array_t arr;
  arr.reserve(clients.size());
  for (const auto& client : clients) {
    const auto [running, pid] = vinput::cli::ResolveClientRuntimeStatus(status_map, client.id);
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
}

void PrintClientListTable(Formatter& fmt, ClientKind kind, const std::vector<ClientSpec>& clients,
                          const vinput::cli::ClientStatusMap& status_map) {
  if (clients.empty()) {
    fmt.PrintInfo(::fmt::sprintf(_("No %ss configured."), KindLabel(kind)));
    return;
  }

  const std::vector<std::string> headers = {_("ID"), _("STATUS"), _("PID"), _("AUTOSTART"),
                                            _("COMMAND")};
  std::vector<std::vector<std::string>> rows;
  rows.reserve(clients.size());
  for (const auto& client : clients) {
    const auto [running, pid] = vinput::cli::ResolveClientRuntimeStatus(status_map, client.id);
    rows.push_back({
        client.id,
        running ? _("running") : _("stopped"),
        pid > 0 ? std::to_string(pid) : "-",
        client.autoStart ? _("enabled") : _("disabled"),
        client.command,
    });
  }
  fmt.PrintTable(headers, rows);
}

struct ListenContext {
  std::string type_filter;
  bool json_output = false;
};

int OnRecognitionPartial(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
  auto* lctx = static_cast<ListenContext*>(userdata);
  if (lctx->type_filter != "all" && lctx->type_filter != "partial") {
    return 0;
  }
  const char* text = "";
  if (sd_bus_message_read(m, "s", &text) < 0 || text == nullptr) {
    return 0;
  }
  if (lctx->json_output) {
    std::cout << vinput::client::SerializePartialEvent(text, true) << '\n';
  } else {
    std::cout << text << '\n';
  }
  std::cout.flush();
  return 0;
}

int OnRecognitionResult(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
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
    std::cout << vinput::client::SerializeCommittedEvent(payload, true) << '\n';
  } else {
    std::cout << payload.commitText << '\n';
  }
  std::cout.flush();
  return 0;
}

int OnStatusChanged(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
  auto* lctx = static_cast<ListenContext*>(userdata);
  if (lctx->type_filter != "all" && lctx->type_filter != "status") {
    return 0;
  }
  const char* status = "";
  if (sd_bus_message_read(m, "s", &status) < 0 || status == nullptr) {
    return 0;
  }
  if (lctx->json_output) {
    std::cout << vinput::client::SerializeStatusEvent(status, true) << '\n';
  } else {
    std::cout << status << '\n';
  }
  std::cout.flush();
  return 0;
}

struct ListenSubscriptions {
  sd_bus_slot* slot_partial = nullptr;
  sd_bus_slot* slot_result = nullptr;
  sd_bus_slot* slot_status = nullptr;

  ~ListenSubscriptions() {
    if (slot_partial != nullptr) {
      sd_bus_slot_unref(slot_partial);
    }
    if (slot_result != nullptr) {
      sd_bus_slot_unref(slot_result);
    }
    if (slot_status != nullptr) {
      sd_bus_slot_unref(slot_status);
    }
  }

  ListenSubscriptions() = default;
  ListenSubscriptions(const ListenSubscriptions&) = delete;
  ListenSubscriptions& operator=(const ListenSubscriptions&) = delete;
  ListenSubscriptions(ListenSubscriptions&&) = delete;
  ListenSubscriptions& operator=(ListenSubscriptions&&) = delete;
};

bool SubscribeSignals(sd_bus* bus, ListenContext& lctx, ListenSubscriptions& subs) {
  int ret = 0;
  if (lctx.type_filter == "all" || lctx.type_filter == "partial") {
    ret = sd_bus_match_signal(bus, &subs.slot_partial, vinput::dbus::kBusName,
                              vinput::dbus::kObjectPath, vinput::dbus::kInterface,
                              vinput::dbus::kSignalRecognitionPartial, OnRecognitionPartial, &lctx);
  }
  if (ret >= 0 && (lctx.type_filter == "all" || lctx.type_filter == "committed")) {
    ret = sd_bus_match_signal(bus, &subs.slot_result, vinput::dbus::kBusName,
                              vinput::dbus::kObjectPath, vinput::dbus::kInterface,
                              vinput::dbus::kSignalRecognitionResult, OnRecognitionResult, &lctx);
  }
  if (ret >= 0 && (lctx.type_filter == "all" || lctx.type_filter == "status")) {
    ret = sd_bus_match_signal(bus, &subs.slot_status, vinput::dbus::kBusName,
                              vinput::dbus::kObjectPath, vinput::dbus::kInterface,
                              vinput::dbus::kSignalStatusChanged, OnStatusChanged, &lctx);
  }
  return ret >= 0;
}

int ProcessListenLoop(sd_bus* bus) {
  while (!ListenInterrupted().load(std::memory_order_relaxed)) {
    const int r = sd_bus_process(bus, nullptr);
    if (r > 0) {
      continue;
    }
    if (r < 0) {
      return r;
    }
    const int w = sd_bus_wait(bus, 250000); // 250ms
    if (w < 0 && w != -EINTR) {
      return w;
    }
  }
  return 0;
}

} // namespace

int RunClientList(ClientKind kind, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const auto& clients = GetClientList(config, kind);
  vinput::cli::DbusClient dbus;
  const auto status_map = dbus.GetClientStatusesMap();

  if (ctx.json_output) {
    PrintClientListJson(fmt, clients, status_map);
    return 0;
  }

  PrintClientListTable(fmt, kind, clients, status_map);
  return 0;
}

int RunClientStart(ClientKind kind, const std::string& id, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  if (!HasClientWithId(config, kind, id)) {
    const std::string err =
        ::fmt::sprintf(_("'%s' is not configured as a %s."), id, KindLabel(kind));
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", err}});
    } else {
      fmt.PrintError(err);
    }
    return 1;
  }

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

int RunClientStop(ClientKind kind, const std::string& id, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  if (!HasClientWithId(config, kind, id)) {
    const std::string err =
        ::fmt::sprintf(_("'%s' is not configured as a %s."), id, KindLabel(kind));
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", err}});
    } else {
      fmt.PrintError(err);
    }
    return 1;
  }

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

int RunListen(std::string_view type_filter, Formatter& /*fmt*/, const CliContext& ctx) {
  ListenContext lctx;
  lctx.type_filter = type_filter.empty() ? "committed" : std::string(type_filter);
  lctx.json_output = ctx.json_output;

  sd_bus* bus = nullptr;
  if (sd_bus_default_user(&bus) < 0 || bus == nullptr) {
    std::cerr << _("Failed to connect to user D-Bus.") << '\n';
    return 1;
  }

  ListenSubscriptions subs;
  if (!SubscribeSignals(bus, lctx, subs)) {
    std::cerr << _("Failed to subscribe to daemon signals.") << '\n';
    sd_bus_flush_close_unref(bus);
    return 1;
  }

  ListenInterrupted().store(false, std::memory_order_relaxed);
  const auto prev_int = std::signal(SIGINT, OnListenSignal);
  const auto prev_term = std::signal(SIGTERM, OnListenSignal);

  const int loop_error = ProcessListenLoop(bus);

  std::signal(SIGINT, prev_int);
  std::signal(SIGTERM, prev_term);
  sd_bus_flush_close_unref(bus);

  if (loop_error < 0 && !ListenInterrupted().load(std::memory_order_relaxed)) {
    std::cerr << _("D-Bus connection error during listen.") << '\n';
    return 1;
  }
  return 0;
}

} // namespace vinput::cli::control
