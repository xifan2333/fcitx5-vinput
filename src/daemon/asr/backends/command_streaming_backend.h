#pragma once

#include <memory>
#include <string>

#include "common/config/core_config_types.h"

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::notify {
class NotificationBridge;
}

namespace vinput::daemon::asr {

std::unique_ptr<AsrBackend>
CreateCommandStreamingBackend(const CommandAsrProvider& provider, std::string* error,
                              vinput::notify::NotificationBridge* notification_bridge = nullptr);

} // namespace vinput::daemon::asr
