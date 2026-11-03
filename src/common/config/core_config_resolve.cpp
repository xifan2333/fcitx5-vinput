#include <string>
#include <variant>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"

bool SetPreferredLocalModel(CoreConfig* config, const std::string& model, std::string* error) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "Config is null.";
    }
    return false;
  }

  const AsrProvider* provider = CoreConfigView(*config).PreferredLocalAsrProvider();
  if (provider == nullptr) {
    if (error != nullptr) {
      *error = "No local ASR provider configured.";
    }
    return false;
  }

  const std::string providerId = AsrProviderId(*provider);
  for (auto& candidate : config->asr.providers) {
    if (AsrProviderId(candidate) == providerId) {
      if (auto* local = std::get_if<LocalAsrProvider>(&candidate)) {
        local->model = model;
        return true;
      }
    }
  }

  if (error != nullptr) {
    *error = "Local ASR provider not found.";
  }
  return false;
}
