#pragma once
#include <filesystem>
#include <string>

namespace vinput::config {

// Dotpath access for extra.* (config.json) paths
// Example: "extra.core.active_model" → reads/writes config.json ["core"]["active_model"]
bool GetConfigValue(const std::string& dotpath, std::string* value, std::string* error);
bool SetConfigValue(const std::string& dotpath, const std::string& value, std::string* error);

// Returns the file path for the given config target
// "extra" → config.json path, "fcitx" → vinput.conf path
std::filesystem::path GetEditTarget(const std::string& target);

} // namespace vinput::config
