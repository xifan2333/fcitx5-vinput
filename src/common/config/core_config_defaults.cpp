#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"

#include "config.h"

namespace {

std::filesystem::path BundledDefaultCoreConfigPath() {
  std::filesystem::path source = VINPUT_DEFAULT_CORE_CONFIG_SOURCE_PATH;
  std::error_code ec;
  if (std::filesystem::exists(source, ec) && !ec) {
    return source;
  }
  return {VINPUT_DEFAULT_CORE_CONFIG_INSTALL_PATH};
}

} // namespace

bool LoadBundledDefaultCoreConfig(CoreConfig* config, std::string* error) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "Config is null.";
    }
    return false;
  }

  const auto path = BundledDefaultCoreConfigPath();
  std::ifstream file(path);
  if (!file.is_open()) {
    if (error != nullptr) {
      *error = "Failed to open bundled default config: " + path.string();
    }
    return false;
  }

  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  if (!CoreConfigFromJson(text, config, error)) {
    if (error != nullptr) {
      *error = "Failed to parse bundled default config: " + *error;
    }
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}
