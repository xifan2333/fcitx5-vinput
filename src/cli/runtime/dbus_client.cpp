#include "cli/runtime/dbus_client.h"

#include <cstdint>
#include <string>
#include <systemd/sd-bus.h>
#include <utility>
#include <vector>

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
  std::vector<std::string> endpoints;
  if (sd_bus_message_enter_container(reply.get(), 'a', "s") >= 0) {
    const char* endpoint = nullptr;
    while (sd_bus_message_read_basic(reply.get(), 's', static_cast<void*>(&endpoint)) > 0) {
      endpoints.emplace_back((endpoint != nullptr) ? endpoint : "");
    }
    sd_bus_message_exit_container(reply.get());
  }
  if (state != nullptr) {
    state->target_provider_id = (target_provider != nullptr) ? target_provider : "";
    state->target_model_id = (target_model != nullptr) ? target_model : "";
    state->effective_provider_id = (effective_provider != nullptr) ? effective_provider : "";
    state->effective_model_id = (effective_model != nullptr) ? effective_model : "";
    state->last_error = (last_error != nullptr) ? last_error : "";
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

bool DbusClient::StartAdapter(const std::string& adapter_id, std::string* error) {
  return CallDaemonStringArg(bus_, vinput::dbus::kMethodStartAdapter, adapter_id, error);
}

bool DbusClient::StopAdapter(const std::string& adapter_id, std::string* error) {
  return CallDaemonStringArg(bus_, vinput::dbus::kMethodStopAdapter, adapter_id, error);
}

} // namespace vinput::cli
