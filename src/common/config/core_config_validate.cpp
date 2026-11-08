#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/scene/postprocess_scene.h"

namespace {

bool SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

template <typename T>
bool CheckUniqueId(const T& items, std::string_view label, std::string* error) {
  std::set<std::string> seen;
  for (const auto& item : items) {
    if (item.id.empty()) {
      return SetError(error, std::string(label) + " id must not be empty.");
    }
    if (!seen.insert(item.id).second) {
      return SetError(error,
                      std::string("Duplicate ") + std::string(label) + " id '" + item.id + "'.");
    }
  }
  return true;
}

bool ValidateAsrConfig(const CoreConfig& config, std::string* error) {
  std::set<std::string> seen;
  for (const auto& provider : config.asr.providers) {
    const std::string& id = AsrProviderId(provider);
    if (id.empty()) {
      return SetError(error, "ASR provider id must not be empty.");
    }
    if (!seen.insert(id).second) {
      return SetError(error, "Duplicate ASR provider id '" + id + "'.");
    }
    if (const auto* cmd = std::get_if<CommandAsrProvider>(&provider)) {
      if (cmd->command.empty()) {
        return SetError(error, "Command ASR provider '" + id + "' must not have an empty command.");
      }
    }
  }

  if (!config.asr.activeProvider.empty() &&
      CoreConfigView(config).AsrProvider(config.asr.activeProvider) == nullptr) {
    return SetError(error,
                    "Active ASR provider '" + config.asr.activeProvider + "' does not exist.");
  }
  return true;
}

bool ValidateSingleSceneConfig(const vinput::scene::Definition& scene, const CoreConfigView& view,
                               std::string* error) {
  std::string sceneError;
  if (!vinput::scene::ValidateDefinition(scene, &sceneError)) {
    return SetError(error, "Invalid scene '" + scene.id + "': " + sceneError);
  }
  if (!scene.provider_id.empty() && scene.model.empty()) {
    if (view.LlmProvider(scene.provider_id) != nullptr) {
      return SetError(error, "Invalid scene '" + scene.id +
                                 "': Scene model must be configured for LLM providers.");
    }
  }
  return true;
}

bool ValidateScenesConfig(const CoreConfig& config, std::string* error) {
  bool hasRaw = false;
  bool hasCommand = false;
  std::set<std::string> seen;
  const CoreConfigView view(config);

  for (const auto& scene : config.scenes.definitions) {
    if (!ValidateSingleSceneConfig(scene, view, error)) {
      return false;
    }
    if (!seen.insert(scene.id).second) {
      return SetError(error, "Duplicate scene id '" + scene.id + "'.");
    }
    if (scene.id == vinput::scene::kRawSceneId) {
      hasRaw = true;
    } else if (scene.id == vinput::scene::kCommandSceneId) {
      hasCommand = true;
    }
  }

  if (!hasRaw) {
    return SetError(error, "Builtin scene '__raw__' is required.");
  }
  if (!hasCommand) {
    return SetError(error, "Builtin scene '__command__' is required.");
  }

  if (!config.scenes.activeScene.empty() &&
      vinput::scene::Find(
          {.activeSceneId = config.scenes.activeScene, .scenes = config.scenes.definitions},
          config.scenes.activeScene) == nullptr) {
    return SetError(error, "Active scene '" + config.scenes.activeScene + "' does not exist.");
  }
  return true;
}

} // namespace

bool ValidateCoreConfig(const CoreConfig& config, std::string* error) {
  if (!CheckUniqueId(config.llm.providers, "LLM provider", error)) {
    return false;
  }

  if (!CheckUniqueId(config.llm.adapters, "LLM adapter", error)) {
    return false;
  }

  if (!ValidateAsrConfig(config, error)) {
    return false;
  }

  if (!ValidateScenesConfig(config, error)) {
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}
