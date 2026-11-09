#include "cli/runtime/dbus_client.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <systemd/sd-bus.h>
#include <utility>
#include <vector>

#include "common/client/client_manager.h"
#include "common/dbus/dbus_interface.h"
#include "common/runtime/runtime_defaults.h"

namespace vinput::cli {

namespace {

struct ScopedMessage {
  sd_bus_message* msg = nullptr;
  ~ScopedMessage() {
    if (msg != nullptr) {
      sd_bus_message_unref(msg);
    }
  }

  ScopedMessage() = default;
  ScopedMessage(const ScopedMessage&) = delete;
  ScopedMessage& operator=(const ScopedMessage&) = delete;
  ScopedMessage(ScopedMessage&& other) noexcept : msg(other.msg) { other.msg = nullptr; }
  ScopedMessage& operator=(ScopedMessage&& other) noexcept {
    if (this != &other) {
      if (msg != nullptr) {
        sd_bus_message_unref(msg);
      }
      msg = other.msg;
      other.msg = nullptr;
    }
    return *this;
  }

  [[nodiscard]] sd_bus_message* get() const noexcept { return msg; }
};

struct ScopedError {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  ~ScopedError() { sd_bus_error_free(&error); }
  ScopedError() = default;
  ScopedError(const ScopedError&) = delete;
  ScopedError& operator=(const ScopedError&) = delete;
  ScopedError(ScopedError&&) = delete;
  ScopedError& operator=(ScopedError&&) = delete;
};

template <typename... Args>
bool CallBusMethod(sd_bus* bus, const char* destination, const char* path, const char* interface,
                   const char* member, ScopedMessage* reply, std::string* error, const char* types,
                   Args... args) {
  if (bus == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  ScopedError err;
  sd_bus_message* raw_reply = nullptr;
  const int r = sd_bus_call_method(bus, destination, path, interface, member, &err.error,
                                   &raw_reply, types, args...);
  if (reply != nullptr) {
    reply->msg = raw_reply;
  } else if (raw_reply != nullptr) {
    sd_bus_message_unref(raw_reply);
  }

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.error.message != nullptr) ? err.error.message : "D-Bus call failed";
    }
    return false;
  }
  return true;
}

template <typename... Args>
bool CallDaemonMethod(sd_bus* bus, const char* member, ScopedMessage* reply, std::string* error,
                      const char* types, Args... args) {
  return CallBusMethod(bus, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                       vinput::dbus::kInterface, member, reply, error, types, args...);
}

bool CallDaemonVoid(sd_bus* bus, const char* member, std::string* error) {
  return CallDaemonMethod(bus, member, nullptr, error, "");
}

bool CallDaemonStringArg(sd_bus* bus, const char* member, const std::string& arg,
                         std::string* error) {
  return CallDaemonMethod(bus, member, nullptr, error, "s", arg.c_str());
}

std::string SafeString(const char* str) {
  return (str != nullptr) ? str : "";
}

std::vector<std::string> ReadStringArray(sd_bus_message* msg) {
  std::vector<std::string> result;
  if (sd_bus_message_enter_container(msg, 'a', "s") >= 0) {
    const char* item = nullptr;
    while (sd_bus_message_read_basic(msg, 's', static_cast<void*>(&item)) > 0) {
      result.emplace_back(SafeString(item));
    }
    sd_bus_message_exit_container(msg);
  }
  return result;
}

bool ReadClientStatusEntry(sd_bus_message* msg, vinput::dbus::ClientStatus& item) {
  const char* id = nullptr;
  int running = 0;
  int64_t pid = -1;
  if (sd_bus_message_read(msg, "sbx", &id, &running, &pid) < 0) {
    return false;
  }
  item.id = SafeString(id);
  item.running = (running != 0);
  item.pid = pid;
  return true;
}

bool ReadClientStatusArray(sd_bus_message* msg, std::vector<vinput::dbus::ClientStatus>& statuses,
                           std::string* error) {
  if (sd_bus_message_enter_container(msg, 'a', "(sbx)") <= 0) {
    if (error != nullptr) {
      *error = "failed to enter client status array";
    }
    return false;
  }

  int r = 0;
  while ((r = sd_bus_message_enter_container(msg, 'r', "sbx")) > 0) {
    vinput::dbus::ClientStatus item;
    const bool ok = ReadClientStatusEntry(msg, item);
    sd_bus_message_exit_container(msg);
    if (!ok) {
      statuses.clear();
      sd_bus_message_exit_container(msg);
      if (error != nullptr) {
        *error = "failed to read client status element";
      }
      return false;
    }
    statuses.push_back(std::move(item));
  }
  sd_bus_message_exit_container(msg);

  if (r < 0) {
    statuses.clear();
    if (error != nullptr) {
      *error = "failed to enter client status element";
    }
    return false;
  }
  return true;
}

} // namespace

DbusClient::DbusClient() {
  const int r = sd_bus_open_user(&bus_);
  if (r < 0) {
    bus_ = nullptr;
    return;
  }
  sd_bus_set_method_call_timeout(bus_,
                                 static_cast<uint64_t>(vinput::runtime::kDbusCallTimeoutUsec));
}

DbusClient::~DbusClient() {
  if (bus_ != nullptr) {
    sd_bus_unref(bus_);
  }
}

bool DbusClient::IsDaemonRunning(std::string* error) {
  ScopedMessage reply;
  if (!CallBusMethod(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                     "NameHasOwner", &reply, error, "s", vinput::dbus::kBusName)) {
    return false;
  }

  int result = 0;
  sd_bus_message_read(reply.get(), "b", &result);
  return result != 0;
}

bool DbusClient::GetDaemonStatus(std::string* status, std::string* error) {
  ScopedMessage reply;
  if (!CallDaemonMethod(bus_, vinput::dbus::kMethodGetStatus, &reply, error, "")) {
    return false;
  }

  const char* str = nullptr;
  sd_bus_message_read(reply.get(), "s", &str);
  if ((status != nullptr) && (str != nullptr)) {
    *status = str;
  }
  return true;
}

bool DbusClient::GetAsrBackendState(vinput::dbus::AsrBackendState* state, std::string* error) {
  ScopedMessage reply;
  if (!CallDaemonMethod(bus_, vinput::dbus::kMethodGetAsrBackendState, &reply, error, "")) {
    return false;
  }

  const char* target_provider = "";
  const char* target_model = "";
  const char* effective_provider = "";
  const char* effective_model = "";
  const char* last_error = "";
  int reload_in_progress = 0;
  int has_effective_backend = 0;
  sd_bus_message_read(reply.get(), "sssssbb", static_cast<void*>(&target_provider),
                      static_cast<void*>(&target_model), static_cast<void*>(&effective_provider),
                      static_cast<void*>(&effective_model), static_cast<void*>(&last_error),
                      &reload_in_progress, &has_effective_backend);
  std::vector<std::string> endpoints = ReadStringArray(reply.get());
  if (state != nullptr) {
    state->target_provider_id = SafeString(target_provider);
    state->target_model_id = SafeString(target_model);
    state->effective_provider_id = SafeString(effective_provider);
    state->effective_model_id = SafeString(effective_model);
    state->last_error = SafeString(last_error);
    state->reload_in_progress = reload_in_progress != 0;
    state->has_effective_backend = has_effective_backend != 0;
    state->remote_endpoints = std::move(endpoints);
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool DbusClient::StartRecording(std::string* error) {
  return CallDaemonVoid(bus_, vinput::dbus::kMethodStartRecording, error);
}

bool DbusClient::StartCommandRecording(const std::string& selected_text, std::string* error) {
  return CallDaemonStringArg(bus_, vinput::dbus::kMethodStartCommandRecording, selected_text,
                             error);
}

bool DbusClient::StopRecording(const std::string& scene_id, std::string* error) {
  return CallDaemonStringArg(bus_, vinput::dbus::kMethodStopRecording, scene_id, error);
}

bool DbusClient::ReloadAsrBackend(std::string* error) {
  return CallDaemonVoid(bus_, vinput::dbus::kMethodReloadAsrBackend, error);
}

bool DbusClient::StartClient(const std::string& client_id, std::string* error) {
  return CallDaemonStringArg(bus_, vinput::dbus::kMethodStartClient, client_id, error);
}

bool DbusClient::StopClient(const std::string& client_id, std::string* error) {
  return CallDaemonStringArg(bus_, vinput::dbus::kMethodStopClient, client_id, error);
}

bool DbusClient::GetClientStatuses(std::vector<vinput::dbus::ClientStatus>* statuses,
                                   std::string* error) {
  if (statuses == nullptr) {
    if (error != nullptr) {
      *error = "null statuses pointer";
    }
    return false;
  }
  statuses->clear();

  ScopedMessage reply;
  if (!CallDaemonMethod(bus_, vinput::dbus::kMethodGetClientStatuses, &reply, error, "")) {
    return false;
  }

  if (!ReadClientStatusArray(reply.get(), *statuses, error)) {
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

ClientStatusMap DbusClient::GetClientStatusesMap(std::string* error) {
  ClientStatusMap map;
  if (!IsDaemonRunning(error)) {
    return map;
  }
  std::vector<vinput::dbus::ClientStatus> statuses;
  if (GetClientStatuses(&statuses, error)) {
    for (auto& s : statuses) {
      map.emplace(s.id, std::move(s));
    }
  }
  return map;
}

std::pair<bool, pid_t> ResolveClientRuntimeStatus(const ClientStatusMap& status_map,
                                                  std::string_view client_id) {
  if (const auto it = status_map.find(std::string(client_id)); it != status_map.end()) {
    return {it->second.running, static_cast<pid_t>(it->second.pid)};
  }
  const auto pid = vinput::client::GetPid(client_id);
  return {pid > 0, pid};
}

} // namespace vinput::cli
