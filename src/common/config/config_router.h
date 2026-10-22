#pragma once
#include <filesystem>
#include <string>

#include "common/utils/expected.h"

namespace vinput::config {

// JSON Pointer (RFC 6901) access to core config.
// Example: "/global/capture_device", "/asr/active_provider"
Expected<std::string> GetConfigValue(const std::string& pointer);
Expected<void> SetConfigValue(const std::string& pointer, const std::string& value);

// Returns the file path for the given editable config target.
// "core" -> config.json path, "fcitx" -> vinput.conf path
std::filesystem::path GetEditTarget(const std::string& target);

} // namespace vinput::config
