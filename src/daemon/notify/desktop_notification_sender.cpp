#include "daemon/notify/desktop_notification_sender.h"

#include <cstdint>
#include <string>
#include <systemd/sd-bus.h>

#include "common/notify/notification_bridge.h"
#include "common/utils/debug_log.h"

namespace vinput::daemon::notify {

const char* NotificationIcon(vinput::notify::NotificationLevel level) noexcept {
  switch (level) {
  case vinput::notify::NotificationLevel::Information:
    return "dialog-information";
  case vinput::notify::NotificationLevel::Warning:
    return "dialog-warning";
  case vinput::notify::NotificationLevel::Error:
    return "dialog-error";
  }
  return "dialog-information";
}

std::int32_t NotificationTimeoutMs(vinput::notify::NotificationLevel level) noexcept {
  switch (level) {
  case vinput::notify::NotificationLevel::Information:
    return 3000;
  case vinput::notify::NotificationLevel::Warning:
    return 4000;
  case vinput::notify::NotificationLevel::Error:
    return 5000;
  }
  return 3000;
}

// Dispatches a notification to org.freedesktop.Notifications.Notify.
// Design Note:
// Uses sd_bus_message_new_method_call + sd_bus_message_set_expect_reply(m, 0) + sd_bus_send
// rather than sd_bus_call_method_async. This fire-and-forget pattern avoids round-trip D-Bus
// reply traffic and callback slot tracking for unreplied popups, and accommodates the complex
// container signature "susssasa{sv}i" without varargs limitations.
bool SendDesktopNotification(sd_bus* bus, const vinput::notify::Notification& notification) {
  if (bus == nullptr) {
    return false;
  }

  sd_bus_message* m = nullptr;
  int r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.Notifications",
                                         "/org/freedesktop/Notifications",
                                         "org.freedesktop.Notifications", "Notify");
  if (r < 0 || m == nullptr) {
    VLOG_WARN("failed to create Notify method call for '{}': {}", notification.title, r);
    return false;
  }

  r = sd_bus_message_set_expect_reply(m, 0);
  if (r < 0) {
    VLOG_WARN("failed to set NO_REPLY_EXPECTED on notification '{}': {}", notification.title, r);
    sd_bus_message_unref(m);
    return false;
  }

  const char* icon = NotificationIcon(notification.level);
  const auto timeout_ms = NotificationTimeoutMs(notification.level);

  // Signature: "susssasa{sv}i"
  // 1. app_name (s)
  // 2. replaces_id (u)
  // 3. app_icon (s)
  // 4. summary (s)
  // 5. body (s)
  r = sd_bus_message_append(m, "susss", "fcitx5-vinput", static_cast<std::uint32_t>(0), icon,
                            notification.title.c_str(), notification.body.c_str());
  if (r < 0) {
    VLOG_WARN("failed to append notification payload for '{}': {}", notification.title, r);
    sd_bus_message_unref(m);
    return false;
  }

  // 6. actions: empty array (as)
  r = sd_bus_message_open_container(m, 'a', "s");
  if (r < 0) {
    sd_bus_message_unref(m);
    return false;
  }
  r = sd_bus_message_close_container(m);
  if (r < 0) {
    sd_bus_message_unref(m);
    return false;
  }

  // 7. hints: empty dict (a{sv})
  r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (r < 0) {
    sd_bus_message_unref(m);
    return false;
  }
  r = sd_bus_message_close_container(m);
  if (r < 0) {
    sd_bus_message_unref(m);
    return false;
  }

  // 8. expire_timeout (i)
  r = sd_bus_message_append(m, "i", timeout_ms);
  if (r < 0) {
    sd_bus_message_unref(m);
    return false;
  }

  r = sd_bus_send(bus, m, nullptr);
  sd_bus_message_unref(m);
  if (r < 0) {
    VLOG_WARN("failed to send desktop notification '{}': {}", notification.title, r);
    return false;
  }
  return true;
}

} // namespace vinput::daemon::notify
