#pragma once

#include <string_view>

#include "common/asr/recognition_result.h"
#include "common/dbus/dbus_interface.h"
#include "common/notify/notification_bridge.h"

namespace vinput::daemon::runtime {

// Narrow output sink injected into SessionLane so each lane role (primary IME,
// headless client, or fd stream) routes status, partials, results, and desktop
// notifications without branching inside SessionLane.
class LaneEventSink {
public:
  LaneEventSink() = default;
  virtual ~LaneEventSink() = default;

  LaneEventSink(const LaneEventSink&) = delete;
  LaneEventSink& operator=(const LaneEventSink&) = delete;
  LaneEventSink(LaneEventSink&&) = delete;
  LaneEventSink& operator=(LaneEventSink&&) = delete;

  virtual void OnStatusChanged(vinput::dbus::Status status, bool ime) = 0;
  virtual void OnPartialText(std::string_view text, bool ime) = 0;
  virtual void OnRecognitionResult(const vinput::result::Payload& payload, bool ime) = 0;
  virtual void OnNotification(const vinput::notify::Notification& notification) = 0;
};

} // namespace vinput::daemon::runtime
