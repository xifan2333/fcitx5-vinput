#include "cli/control/client_actions.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fmt/printf.h>
#include <glaze/json/json_t.hpp>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

extern "C" {
#include <signal.h>
#include <systemd/sd-bus.h>
}

#include "common/asr/recognition_result.h"
#include "common/client/client_event.h"
#include "common/client/client_manager.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/registry/installed_manifest.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/string_utils.h"

#include "cli/runtime/dbus_client.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/cli_helpers.h"
#include "cli/utils/editor_utils.h"
#include "cli/utils/formatter.h"
#include "cli/utils/resource_utils.h"

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

constexpr vinput::script::Kind ToScriptKind(ClientKind kind) noexcept {
  return kind == ClientKind::Trigger ? vinput::script::Kind::kTrigger
                                     : vinput::script::Kind::kReceiver;
}

const std::vector<ClientSpec>& GetClientList(const CoreConfig& config, ClientKind kind) {
  return kind == ClientKind::Trigger ? config.triggers : config.receivers;
}

bool HasClientWithId(const CoreConfig& config, ClientKind kind, std::string_view id) {
  const auto& list = GetClientList(config, kind);
  return std::ranges::any_of(list, [&](const auto& client) { return client.id == id; });
}

void PrintClientListJson(Formatter& fmt, const std::vector<ClientSpec>& clients,
                         const vinput::cli::ClientStatusMap& status_map,
                         const vinput::cli::ResourceDisplayMap& display_map) {
  glz::json_t::array_t arr;
  arr.reserve(clients.size());
  for (const auto& client : clients) {
    const auto [running, pid] = vinput::cli::ResolveClientRuntimeStatus(status_map, client.id);
    glz::json_t entry = {
        {"id", vinput::cli::HumanizeResourceId(display_map, client.id)},
        {"machine_id", client.id},
        {"running", running},
        {"pid", pid > 0 ? glz::json_t(pid) : glz::json_t(nullptr)},
        {"auto_start", client.autoStart},
        {"command", client.command},
        {"args", vinput::json::FromStrings(client.args)},
        {"env", vinput::json::FromStringMap(client.env)},
    };
    arr.emplace_back(std::move(entry));
  }
  fmt.PrintJson(glz::json_t(std::move(arr)));
}

void PrintClientListTable(Formatter& fmt, ClientKind kind, const std::vector<ClientSpec>& clients,
                          const vinput::cli::ClientStatusMap& status_map,
                          const vinput::cli::ResourceDisplayMap& display_map) {
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
        vinput::cli::HumanizeResourceId(display_map, client.id),
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

std::string ResolveClientSelector(const CoreConfig& config, ClientKind kind,
                                  const std::string& selector, std::string* error) {
  return kind == ClientKind::Trigger
             ? vinput::cli::ResolveInstalledTriggerSelector(config, selector, error)
             : vinput::cli::ResolveInstalledReceiverSelector(config, selector, error);
}

template <typename ActionFn>
int RunClientLifecycleAction(ClientKind kind, const std::string& id, std::string_view action_name,
                             const std::string& success_msg, const ActionFn& action, Formatter& fmt,
                             const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  std::string resolve_error;
  const std::string resolved_id = ResolveClientSelector(config, kind, id, &resolve_error);
  if (resolved_id.empty()) {
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", resolve_error}});
    } else {
      fmt.PrintError(resolve_error);
    }
    return 1;
  }
  if (!HasClientWithId(config, kind, resolved_id)) {
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
  if (!action(dbus, resolved_id, &error)) {
    if (ctx.json_output) {
      fmt.PrintJson({{"ok", false}, {"error", error}});
    } else {
      fmt.PrintError(error);
    }
    return 1;
  }

  if (ctx.json_output) {
    fmt.PrintJson({{"ok", true}, {"id", resolved_id}, {"action", std::string(action_name)}});
  } else {
    fmt.PrintSuccess(success_msg);
  }
  return 0;
}

const ClientSpec* GetInstalledClientView(const CoreConfig& config, ClientKind kind,
                                         std::string_view id) {
  return kind == ClientKind::Trigger ? CoreConfigView(config).Trigger(id)
                                     : CoreConfigView(config).Receiver(id);
}

bool MaterializeTargetClient(CoreConfig* config, ClientKind kind,
                             const vinput::script::RegistryEntry& entry,
                             const std::filesystem::path& script_path, std::string* error,
                             const std::map<std::string, std::string>& env_overrides = {}) {
  return kind == ClientKind::Trigger
             ? vinput::script::MaterializeTrigger(config, entry, script_path, error, env_overrides)
             : vinput::script::MaterializeReceiver(config, entry, script_path, error,
                                                   env_overrides);
}

void ValidateRequiredClientEnvs(const ClientSpec* installed,
                                const vinput::script::RegistryEntry& entry,
                                const std::string& selector, Formatter& fmt) {
  if (installed == nullptr) {
    return;
  }
  for (const auto& spec : entry.envs) {
    if (!spec.required) {
      continue;
    }
    const auto it = installed->env.find(spec.name);
    if (it == installed->env.end() || std::string(vinput::str::Trim(it->second)).empty()) {
      fmt.PrintWarning(::fmt::sprintf(
          _("Required env '%s' is empty; '%s' cannot start until it is set (re-run with -e "
            "%s=<value>)."),
          spec.name, selector, spec.name));
    }
  }
}

std::filesystem::path ResolveClientScriptPath(const ClientSpec* client,
                                              vinput::script::Kind script_kind,
                                              const std::string& resolved_id) {
  std::error_code ec;
  if (client != nullptr && !client->args.empty()) {
    const std::filesystem::path candidate(client->args.front());
    if (std::filesystem::exists(candidate, ec) && !ec) {
      return candidate;
    }
  }
  return vinput::script::DefaultLocalScriptPath(script_kind, resolved_id);
}

bool UpdateSingleClient(CoreConfig* config, ClientKind kind,
                        const vinput::registry::InstalledManifest& manifest,
                        const vinput::script::RegistryEntry& entry, bool force, Formatter& fmt) {
  const auto script_kind = ToScriptKind(kind);
  return vinput::cli::UpdateRegistryScript(
      manifest, entry, script_kind, force,
      [config, kind, &entry](const std::filesystem::path& path, std::string* error) {
        return MaterializeTargetClient(config, kind, entry, path, error);
      },
      fmt);
}

struct UpdateAllResult {
  bool any_updated = false;
  bool any_failed = false;
};

UpdateAllResult UpdateAllClients(CoreConfig* config, ClientKind kind,
                                 const vinput::registry::InstalledManifest& manifest,
                                 const std::vector<vinput::script::RegistryEntry>& entries,
                                 bool force, Formatter& fmt) {
  UpdateAllResult result;
  const auto& clients = GetClientList(*config, kind);
  for (const auto& client : clients) {
    auto it = std::ranges::find_if(entries, [&](const auto& e) { return e.id == client.id; });
    if (it == entries.end()) {
      continue;
    }
    if (UpdateSingleClient(config, kind, manifest, *it, force, fmt)) {
      result.any_updated = true;
    } else {
      result.any_failed = true;
    }
  }
  return result;
}

} // namespace

int RunClientList(ClientKind kind, bool available, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const auto script_kind = ToScriptKind(kind);

  if (available) {
    return vinput::cli::RunRegistryScriptList(
        config, script_kind,
        [&config, kind](const std::string& id) {
          return kind == ClientKind::Trigger ? CoreConfigView(config).Trigger(id) != nullptr
                                             : CoreConfigView(config).Receiver(id) != nullptr;
        },
        fmt, ctx);
  }

  const auto& clients = GetClientList(config, kind);
  vinput::cli::DbusClient dbus;
  const auto status_map = dbus.GetClientStatusesMap();
  const auto display_map = vinput::cli::FetchScriptDisplayMap(config, script_kind);

  if (ctx.json_output) {
    PrintClientListJson(fmt, clients, status_map, display_map);
    return 0;
  }

  PrintClientListTable(fmt, kind, clients, status_map, display_map);
  return 0;
}

int RunClientInstall(ClientKind kind, const std::string& selector,
                     const std::vector<std::string>& env_overrides, Formatter& fmt,
                     const CliContext& ctx) {
  (void)ctx;
  std::map<std::string, std::string> overrides;
  if (!vinput::cli::ParseEnvOverrides(env_overrides, &overrides, fmt)) {
    return 1;
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const auto script_kind = ToScriptKind(kind);
  const auto entries_opt = vinput::cli::FetchScriptRegistryEntries(config, script_kind, fmt);
  if (!entries_opt) {
    return 1;
  }
  const auto& entries = *entries_opt;

  const auto* entry = vinput::cli::FindRegistryScriptEntry(entries, script_kind, selector, fmt);
  if (entry == nullptr) {
    return 1;
  }

  const auto script_path = vinput::cli::DownloadRegistryScript(*entry, script_kind, fmt);
  if (!script_path) {
    return 1;
  }

  std::string error;
  if (!MaterializeTargetClient(&config, kind, *entry, *script_path, &error, overrides)) {
    fmt.PrintError(error);
    return 1;
  }

  NormalizeCoreConfig(&config);
  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }

  const auto* installed = GetInstalledClientView(config, kind, entry->id);
  ValidateRequiredClientEnvs(installed, *entry, selector, fmt);
  fmt.PrintSuccess(::fmt::sprintf(kind == ClientKind::Trigger ? _("Trigger '%s' added.")
                                                              : _("Receiver '%s' added."),
                                  selector));
  return 0;
}

int RunClientRemove(ClientKind kind, const std::string& selector, Formatter& fmt,
                    const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id = ResolveClientSelector(config, kind, selector, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  auto& clients = kind == ClientKind::Trigger ? config.triggers : config.receivers;
  const auto removed = std::ranges::remove_if(
      clients, [&resolved_id](const ClientSpec& c) { return c.id == resolved_id; });
  if (removed.empty()) {
    fmt.PrintError(::fmt::sprintf(kind == ClientKind::Trigger ? _("Trigger '%s' not found.")
                                                              : _("Receiver '%s' not found."),
                                  selector));
    return 1;
  }
  clients.erase(removed.begin(), removed.end());

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }

  const auto script_kind = ToScriptKind(kind);
  const auto managed_path = vinput::script::DefaultLocalScriptPath(script_kind, resolved_id);
  if (!managed_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(managed_path, ec);
  }
  vinput::script::UnregisterInstalledScript(resolved_id);

  fmt.PrintSuccess(::fmt::sprintf(kind == ClientKind::Trigger ? _("Trigger '%s' removed.")
                                                              : _("Receiver '%s' removed."),
                                  selector));
  return 0;
}

static int RunClientUpdateAll(CoreConfig* config, ClientKind kind,
                              const vinput::registry::InstalledManifest& manifest,
                              const std::vector<vinput::script::RegistryEntry>& entries, bool force,
                              Formatter& fmt) {
  const auto result = UpdateAllClients(config, kind, manifest, entries, force, fmt);
  if (result.any_updated) {
    NormalizeCoreConfig(config);
    if (!SaveConfigOrFail(*config, fmt)) {
      return 1;
    }
  }
  return result.any_failed ? 1 : 0;
}

static int RunClientUpdateSingle(CoreConfig* config, ClientKind kind,
                                 const vinput::registry::InstalledManifest& manifest,
                                 const std::vector<vinput::script::RegistryEntry>& entries,
                                 const std::string& selector, bool force, Formatter& fmt) {
  const auto script_kind = ToScriptKind(kind);
  const auto* entry = vinput::cli::FindRegistryScriptEntry(entries, script_kind, selector, fmt);
  if (entry == nullptr) {
    return 1;
  }

  if (GetInstalledClientView(*config, kind, entry->id) == nullptr) {
    fmt.PrintError(::fmt::sprintf(kind == ClientKind::Trigger
                                      ? _("Trigger '%s' is not installed. Use 'add' instead.")
                                      : _("Receiver '%s' is not installed. Use 'add' instead."),
                                  selector));
    return 1;
  }

  if (!UpdateSingleClient(config, kind, manifest, *entry, force, fmt)) {
    return 1;
  }

  NormalizeCoreConfig(config);
  return SaveConfigOrFail(*config, fmt) ? 0 : 1;
}

int RunClientUpdate(ClientKind kind, const std::string& selector, bool all, bool force,
                    Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  if (!all && selector.empty()) {
    fmt.PrintError(kind == ClientKind::Trigger
                       ? _("Specify a trigger short ID to update, or use --all to update all "
                           "installed triggers.")
                       : _("Specify a receiver short ID to update, or use --all to update all "
                           "installed receivers."));
    return 1;
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const auto script_kind = ToScriptKind(kind);
  const auto entries_opt = vinput::cli::FetchScriptRegistryEntries(config, script_kind, fmt);
  if (!entries_opt) {
    return 1;
  }
  const auto& entries = *entries_opt;
  const auto manifest = vinput::registry::InstalledManifest::Load();

  if (all) {
    return RunClientUpdateAll(&config, kind, manifest, entries, force, fmt);
  }
  return RunClientUpdateSingle(&config, kind, manifest, entries, selector, force, fmt);
}

int RunClientEdit(ClientKind kind, const std::string& selector, Formatter& fmt,
                  const CliContext& ctx) {
  (void)ctx;
  const CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id = ResolveClientSelector(config, kind, selector, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto* client = GetInstalledClientView(config, kind, resolved_id);
  if (client == nullptr) {
    fmt.PrintError(::fmt::sprintf(kind == ClientKind::Trigger ? _("Trigger '%s' not found.")
                                                              : _("Receiver '%s' not found."),
                                  selector));
    return 1;
  }

  const auto script_kind = ToScriptKind(kind);
  const auto script_path = ResolveClientScriptPath(client, script_kind, resolved_id);
  std::error_code ec;
  if (script_path.empty() || !std::filesystem::exists(script_path, ec) || ec) {
    fmt.PrintError(
        ::fmt::sprintf(kind == ClientKind::Trigger
                           ? _("Trigger '%s' does not reference an editable script file.")
                           : _("Receiver '%s' does not reference an editable script file."),
                       selector));
    return 1;
  }

  const int ret = OpenInEditor(script_path);
  if (ret != 0) {
    fmt.PrintError(_("Editor exited with error."));
    return ret;
  }

  fmt.PrintSuccess(::fmt::sprintf(kind == ClientKind::Trigger ? _("Trigger '%s' updated.")
                                                              : _("Receiver '%s' updated."),
                                  selector));
  return 0;
}

int RunClientStart(ClientKind kind, const std::string& id, Formatter& fmt, const CliContext& ctx) {
  return RunClientLifecycleAction(
      kind, id, "start", ::fmt::sprintf(_("Client '%s' started."), id),
      [](vinput::cli::DbusClient& dbus, const std::string& client_id, std::string* error) {
        return dbus.StartClient(client_id, error);
      },
      fmt, ctx);
}

int RunClientStop(ClientKind kind, const std::string& id, Formatter& fmt, const CliContext& ctx) {
  return RunClientLifecycleAction(
      kind, id, "stop", ::fmt::sprintf(_("Client '%s' stopped."), id),
      [](vinput::cli::DbusClient& dbus, const std::string& client_id, std::string* error) {
        return dbus.StopClient(client_id, error);
      },
      fmt, ctx);
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
  const auto prev_int = signal(SIGINT, OnListenSignal);
  const auto prev_term = signal(SIGTERM, OnListenSignal);

  const int loop_error = ProcessListenLoop(bus);

  signal(SIGINT, prev_int);
  signal(SIGTERM, prev_term);
  sd_bus_flush_close_unref(bus);

  if (loop_error < 0 && !ListenInterrupted().load(std::memory_order_relaxed)) {
    std::cerr << _("D-Bus connection error during listen.") << '\n';
    return 1;
  }
  return 0;
}

} // namespace vinput::cli::control
