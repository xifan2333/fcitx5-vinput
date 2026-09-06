#include "common/scene/postprocess_scene.h"

#include <string>
#include <string_view>

#include "common/i18n.h"

namespace vinput::scene {

namespace {

bool HasProvider(const Definition& scene) {
  return !scene.provider_id.empty();
}

bool HasModel(const Definition& scene) {
  return !scene.model.empty();
}

bool SetValidationError(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

const char* BuiltinSceneLabel(std::string_view scene_id) {
  if (scene_id == kRawSceneId) {
    return _("Raw");
  }
  if (scene_id == kCommandSceneId) {
    return _("Command");
  }
  return nullptr;
}

const char* BuiltinSceneLabelFromKey(std::string_view label) {
  if (label == kRawSceneLabelKey) {
    return _("Raw");
  }
  if (label == kCommandSceneLabelKey) {
    return _("Command");
  }
  return nullptr;
}

} // namespace

int NormalizeLlmMaxCandidates(int llm_max_candidates) {
  if (llm_max_candidates < kMinLlmMaxCandidates) {
    return kMinLlmMaxCandidates;
  }
  if (llm_max_candidates > kMaxLlmMaxCandidates) {
    return kMaxLlmMaxCandidates;
  }
  return llm_max_candidates;
}

bool IsBuiltinSceneId(std::string_view scene_id) {
  return scene_id == kRawSceneId || scene_id == kCommandSceneId;
}

bool IsBuiltinSceneLabelKey(std::string_view label) {
  return label == kRawSceneLabelKey || label == kCommandSceneLabelKey;
}

void NormalizeDefinition(Definition* scene) {
  if (!scene) {
    return;
  }
  if (IsBuiltinSceneId(scene->id)) {
    scene->builtin = true;
  }
  scene->llm_max_candidates = NormalizeLlmMaxCandidates(scene->llm_max_candidates);
  if (scene->timeout_ms <= 0) {
    scene->timeout_ms = kDefaultTimeoutMs;
  }
  if (scene->context_lines < 0) {
    scene->context_lines = 0;
  }
}

bool ValidateDefinition(const Definition& scene, std::string* error, bool require_id) {
  if (require_id && scene.id.empty()) {
    return SetValidationError(error, "Scene id must not be empty.");
  }
  if (scene.llm_max_candidates < 0) {
    return SetValidationError(error, "Scene LLM max candidates must be 0 or greater.");
  }
  if (scene.timeout_ms <= 0) {
    return SetValidationError(error, "Scene timeout must be greater than 0.");
  }

  const bool has_provider = HasProvider(scene);
  const bool has_model = HasModel(scene);
  if (has_provider != has_model) {
    return SetValidationError(error, "Scene provider and model must be configured together.");
  }
  return true;
}

const Definition* Find(const Config& config, std::string_view scene_id) {
  for (const auto& scene : config.scenes) {
    if (scene.id == scene_id) {
      return &scene;
    }
  }
  return nullptr;
}

const Definition& Resolve(const Config& config, std::string_view scene_id) {
  if (const auto* scene = Find(config, scene_id)) {
    return *scene;
  }
  if (const auto* scene = Find(config, config.activeSceneId)) {
    return *scene;
  }
  static const Definition empty;
  return empty;
}

std::string DisplayLabel(const Definition& scene) {
  if (const char* builtin_label = BuiltinSceneLabelFromKey(scene.label)) {
    return builtin_label;
  }
  if (!scene.label.empty()) {
    return scene.label;
  }
  if (const char* builtin_label = BuiltinSceneLabel(scene.id)) {
    return builtin_label;
  }
  return scene.id;
}

bool AddScene(Config* config, const Definition& def, std::string* error) {
  Definition normalized = def;
  NormalizeDefinition(&normalized);
  if (!ValidateDefinition(normalized, error)) {
    return false;
  }
  if (Find(*config, normalized.id)) {
    if (error)
      *error = "Scene id '" + normalized.id + "' already exists.";
    return false;
  }
  config->scenes.push_back(std::move(normalized));
  return true;
}

bool UpdateScene(Config* config, const std::string& id, const Definition& def, std::string* error) {
  for (auto& scene : config->scenes) {
    if (scene.id == id) {
      Definition updated = scene;
      updated.label = def.label;
      updated.prompt = def.prompt;
      updated.provider_id = def.provider_id;
      updated.model = def.model;
      updated.context_lines = def.context_lines;
      updated.llm_max_candidates = def.llm_max_candidates;
      updated.timeout_ms = def.timeout_ms;
      updated.raw_cand = def.raw_cand;
      updated.raw_prev = def.raw_prev;
      NormalizeDefinition(&updated);
      if (!ValidateDefinition(updated, error)) {
        return false;
      }
      scene = std::move(updated);
      return true;
    }
  }
  if (error)
    *error = "Scene id '" + id + "' not found.";
  return false;
}

bool RemoveScene(Config* config, const std::string& id, bool force, std::string* error) {
  if (const auto* scene = Find(*config, id); scene && scene->builtin) {
    if (error)
      *error = "Cannot remove built-in scene '" + id + "'.";
    return false;
  }
  if (!force) {
    if (config->activeSceneId == id) {
      if (error)
        *error = "Cannot remove active scene '" + id + "'.";
      return false;
    }
  }
  for (auto it = config->scenes.begin(); it != config->scenes.end(); ++it) {
    if (it->id == id) {
      config->scenes.erase(it);
      if (config->activeSceneId == id) {
        config->activeSceneId = config->scenes.empty() ? "" : config->scenes.front().id;
      }
      return true;
    }
  }
  if (error)
    *error = "Scene id '" + id + "' not found.";
  return false;
}

bool SetActiveScene(Config* config, const std::string& id, std::string* error) {
  if (!Find(*config, id)) {
    if (error)
      *error = "Scene id '" + id + "' not found.";
    return false;
  }
  config->activeSceneId = id;
  return true;
}

int ClearProviderReferences(Config* config, std::string_view provider_id) {
  if (!config || provider_id.empty()) {
    return 0;
  }

  int updated = 0;
  for (auto& scene : config->scenes) {
    if (scene.provider_id != provider_id) {
      continue;
    }
    scene.provider_id.clear();
    scene.model.clear();
    ++updated;
  }
  return updated;
}

} // namespace vinput::scene
