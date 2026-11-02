#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/scene/postprocess_scene.h"

const LlmProvider* ResolveLlmProvider(const CoreConfig& config, const std::string& provider_id) {
  return CoreConfigView(config).LlmProvider(provider_id);
}

const LlmAdapter* ResolveLlmAdapter(const CoreConfig& config, const std::string& adapter_id) {
  return CoreConfigView(config).LlmAdapter(adapter_id);
}

const AsrProvider* ResolveAsrProvider(const CoreConfig& config, const std::string& provider_id) {
  return CoreConfigView(config).AsrProvider(provider_id);
}

const AsrProvider* ResolveActiveAsrProvider(const CoreConfig& config) {
  return CoreConfigView(config).ActiveAsrProvider();
}

const AsrProvider* ResolveActiveLocalAsrProvider(const CoreConfig& config) {
  return CoreConfigView(config).ActiveLocalAsrProvider();
}

const AsrProvider* ResolvePreferredLocalAsrProvider(const CoreConfig& config) {
  return CoreConfigView(config).PreferredLocalAsrProvider();
}

std::string ResolvePreferredLocalModel(const CoreConfig& config) {
  return CoreConfigView(config).PreferredLocalModel();
}

std::vector<std::string> ResolveModelRegistryUrls(const CoreConfig& config) {
  return CoreConfigView(config).ModelRegistryUrls();
}

std::vector<std::string> ResolveAsrProviderRegistryUrls(const CoreConfig& config) {
  return CoreConfigView(config).AsrProviderRegistryUrls();
}

std::vector<std::string> ResolveLlmAdapterRegistryUrls(const CoreConfig& config) {
  return CoreConfigView(config).LlmAdapterRegistryUrls();
}

std::vector<std::string> ResolveRegistryI18nUrls(const CoreConfig& config,
                                                 const std::string& locale) {
  return CoreConfigView(config).RegistryI18nUrls(locale);
}

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

const vinput::scene::Definition* FindCommandScene(const CoreConfig& config) {
  return CoreConfigView(config).CommandScene();
}

std::filesystem::path ResolveModelBaseDir(const CoreConfig&) {
  return CoreConfigView::ModelBaseDir();
}
