#pragma once

#include <cstdint>
#include <systemd/sd-bus.h>

#include "common/notify/notification_bridge.h"

namespace vinput::daemon::notify {

// Maps a notification severity level to its standard freedesktop icon name.
[[nodiscard]] const char* NotificationIcon(vinput::notify::NotificationLevel level) noexcept;

// Maps a notification severity level to its display expiration timeout in milliseconds.
[[nodiscard]] std::int32_t NotificationTimeoutMs(vinput::notify::NotificationLevel level) noexcept;

// Dispatches a notification to org.freedesktop.Notifications.Notify over the user session D-Bus.
// Non-blocking fire-and-forget: sends the message into sd-bus without waiting for a method reply.
bool SendDesktopNotification(sd_bus* bus, const vinput::notify::Notification& notification);

} // namespace vinput::daemon::notify
