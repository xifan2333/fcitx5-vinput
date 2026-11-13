#pragma once

#include <memory>
#include <string>

#include "common/config/core_config_types.h"

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::notify {
class NotificationBridge;
}

namespace vinput::daemon::asr {

// Both command-backed backends (batch and streaming) are constructed from the
// same provider record and reject the same invalid configuration, so the
// validation lives here rather than being restated per translation unit.
template <typename Backend>
std::unique_ptr<AsrBackend>
MakeCommandBackend(const CommandAsrProvider& provider, std::string* error,
                   vinput::notify::NotificationBridge* notification_bridge) {
  if (provider.command.empty()) {
    if (error != nullptr) {
      *error = "Command ASR provider has empty command.";
    }
    return nullptr;
  }

  if (error != nullptr) {
    error->clear();
  }
  return std::make_unique<Backend>(provider, notification_bridge);
}

} // namespace vinput::daemon::asr
