#include "cli/dbus_client.h"
#include "common/dbus_interface.h"
#include <systemd/sd-bus.h>
#include <cstring>

namespace vinput::cli {

DbusClient::DbusClient() {
    int r = sd_bus_open_user(&bus_);
    if (r < 0) {
        bus_ = nullptr;
    }
}

DbusClient::~DbusClient() {
    if (bus_) {
        sd_bus_unref(bus_);
    }
}

bool DbusClient::IsDaemonRunning(std::string* error) {
    if (!bus_) {
        if (error) *error = "D-Bus not connected";
        return false;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    int r = sd_bus_call_method(bus_,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameHasOwner",
        &err, &reply, "s",
        vinput::dbus::kBusName);

    if (r < 0) {
        if (error) {
            *error = err.message ? err.message : "D-Bus call failed";
        }
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        return false;
    }

    int result = 0;
    sd_bus_message_read(reply, "b", &result);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    return result != 0;
}

bool DbusClient::GetDaemonStatus(std::string* status, std::string* error) {
    if (!bus_) {
        if (error) *error = "D-Bus not connected";
        return false;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    int r = sd_bus_call_method(bus_,
        vinput::dbus::kBusName,
        vinput::dbus::kObjectPath,
        vinput::dbus::kInterface,
        vinput::dbus::kMethodGetStatus,
        &err, &reply, "");

    if (r < 0) {
        if (error) {
            *error = err.message ? err.message : "D-Bus call failed";
        }
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        return false;
    }

    const char* str = nullptr;
    sd_bus_message_read(reply, "s", &str);
    if (status && str) *status = str;

    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    return true;
}

} // namespace vinput::cli
