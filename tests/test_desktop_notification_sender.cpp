#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "common/notify/notification_bridge.h"

#include "daemon/notify/desktop_notification_sender.h"

namespace {

void ExpectTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ExpectTrue failed: " << message << "\n";
    std::abort();
  }
}

void TestNotificationIconMapping() {
  using vinput::daemon::notify::NotificationIcon;
  using vinput::notify::NotificationLevel;

  ExpectTrue(std::string_view(NotificationIcon(NotificationLevel::Information)) ==
                 "dialog-information",
             "information icon matches");
  ExpectTrue(std::string_view(NotificationIcon(NotificationLevel::Warning)) == "dialog-warning",
             "warning icon matches");
  ExpectTrue(std::string_view(NotificationIcon(NotificationLevel::Error)) == "dialog-error",
             "error icon matches");
}

void TestNotificationTimeoutMapping() {
  using vinput::daemon::notify::NotificationTimeoutMs;
  using vinput::notify::NotificationLevel;

  ExpectTrue(NotificationTimeoutMs(NotificationLevel::Information) == 3000,
             "information timeout matches");
  ExpectTrue(NotificationTimeoutMs(NotificationLevel::Warning) == 4000, "warning timeout matches");
  ExpectTrue(NotificationTimeoutMs(NotificationLevel::Error) == 5000, "error timeout matches");
}

void TestNullBusSafeHandling() {
  using vinput::daemon::notify::SendDesktopNotification;
  using vinput::notify::Notification;
  using vinput::notify::NotificationLevel;

  const Notification n{
      .title = "Test Title",
      .body = "Test Body",
      .level = NotificationLevel::Warning,
  };

  // Calling with nullptr bus should return false cleanly
  const bool sent = SendDesktopNotification(nullptr, n);
  ExpectTrue(!sent, "null bus returns false cleanly");
}

} // namespace

int main() {
  TestNotificationIconMapping();
  TestNotificationTimeoutMapping();
  TestNullBusSafeHandling();
  return 0;
}
