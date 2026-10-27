#include "cli/runtime/dbus_client.h"

#include <cstdint>
#include <string>
#include <systemd/sd-bus-protocol.h>
#include <systemd/sd-bus.h>
#include <utility>
#include <vector>

#include "common/dbus/dbus_interface.h"
#include "common/runtime/runtime_defaults.h"

namespace vinput::cli {

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
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                   "org.freedesktop.DBus", "NameHasOwner", &err, &reply, "s",
                                   vinput::dbus::kBusName);

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  int result = 0;
  sd_bus_message_read(reply, "b", &result);
  sd_bus_message_unref(reply);
  sd_bus_error_free(&err);

  return result != 0;
}

bool DbusClient::GetDaemonStatus(std::string* status, std::string* error) {
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(bus_, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                                   vinput::dbus::kInterface, vinput::dbus::kMethodGetStatus, &err,
                                   &reply, "");

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  const char* str = nullptr;
  sd_bus_message_read(reply, "s", &str);
  if ((status != nullptr) && (str != nullptr)) {
    *status = str;
  }

  sd_bus_message_unref(reply);
  sd_bus_error_free(&err);

  return true;
}

bool DbusClient::GetAsrBackendState(vinput::dbus::AsrBackendState* state, std::string* error) {
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(bus_, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                                   vinput::dbus::kInterface,
                                   vinput::dbus::kMethodGetAsrBackendState, &err, &reply, "");

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  const char* target_provider = "";
  const char* target_model = "";
  const char* effective_provider = "";
  const char* effective_model = "";
  const char* last_error = "";
  int reload_in_progress = 0;
  int has_effective_backend = 0;
  sd_bus_message_read(reply, "sssssbb", static_cast<void*>(&target_provider),
                      static_cast<void*>(&target_model), static_cast<void*>(&effective_provider),
                      static_cast<void*>(&effective_model), static_cast<void*>(&last_error),
                      &reload_in_progress, &has_effective_backend);
  std::vector<std::string> endpoints;
  if (sd_bus_message_enter_container(reply, 'a', "s") >= 0) {
    const char* endpoint = nullptr;
    while (sd_bus_message_read_basic(reply, 's', static_cast<void*>(&endpoint)) > 0) {
      endpoints.emplace_back((endpoint != nullptr) ? endpoint : "");
    }
    sd_bus_message_exit_container(reply);
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

  sd_bus_message_unref(reply);
  sd_bus_error_free(&err);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool DbusClient::StartRecording(std::string* error) {
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(bus_, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                                   vinput::dbus::kInterface, vinput::dbus::kMethodStartRecording,
                                   &err, &reply, "");

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  sd_bus_error_free(&err);
  return true;
}

bool DbusClient::StartCommandRecording(const std::string& selected_text, std::string* error) {
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(
      bus_, vinput::dbus::kBusName, vinput::dbus::kObjectPath, vinput::dbus::kInterface,
      vinput::dbus::kMethodStartCommandRecording, &err, &reply, "s", selected_text.c_str());

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  sd_bus_error_free(&err);
  return true;
}

bool DbusClient::StopRecording(const std::string& scene_id, std::string* error) {
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(bus_, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                                   vinput::dbus::kInterface, vinput::dbus::kMethodStopRecording,
                                   &err, &reply, "s", scene_id.c_str());

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  sd_bus_error_free(&err);
  return true;
}

bool DbusClient::ReloadAsrBackend(std::string* error) {
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(bus_, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                                   vinput::dbus::kInterface, vinput::dbus::kMethodReloadAsrBackend,
                                   &err, &reply, "");

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  sd_bus_error_free(&err);
  return true;
}

bool DbusClient::StartAdapter(const std::string& adapter_id, std::string* error) {
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(bus_, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                                   vinput::dbus::kInterface, vinput::dbus::kMethodStartAdapter,
                                   &err, &reply, "s", adapter_id.c_str());

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  sd_bus_error_free(&err);
  return true;
}

bool DbusClient::StopAdapter(const std::string& adapter_id, std::string* error) {
  if (bus_ == nullptr) {
    if (error != nullptr) {
      *error = "D-Bus not connected";
    }
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  const int r = sd_bus_call_method(bus_, vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                                   vinput::dbus::kInterface, vinput::dbus::kMethodStopAdapter, &err,
                                   &reply, "s", adapter_id.c_str());

  if (r < 0) {
    if (error != nullptr) {
      *error = (err.message != nullptr) ? err.message : "D-Bus call failed";
    }
    sd_bus_error_free(&err);
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    return false;
  }

  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  sd_bus_error_free(&err);
  return true;
}

} // namespace vinput::cli
