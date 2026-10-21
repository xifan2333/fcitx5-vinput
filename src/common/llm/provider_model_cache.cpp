#include "common/llm/provider_model_cache.h"

#include <cstdint>
#include <ctime>
#include <glaze/core/opts.hpp>
#include <glaze/core/write.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/config/core_config.h"
#include "common/json/json_access.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace vinput::llm {

namespace {

// The cache is keyed by runtime provider ids, so its shape stays dynamic.
constexpr auto kCacheWriteOpts = glz::opts{.prettify = uint8_t{1}, .indentation_width = 2};

glz::json_t EmptyObject() {
  return {glz::json_t::object_t{}};
}

bool ReadCacheFile(glz::json_t* out) {
  if (out == nullptr) {
    return false;
  }
  *out = EmptyObject();

  const auto path = vinput::path::ProviderModelCachePath();
  const auto content = vinput::file::ReadTextFile(path);
  if (!content) {
    return false;
  }
  if (bool(glz::read_json(*out, *content)) || !out->is_object()) {
    *out = EmptyObject();
    return false;
  }
  return true;
}

bool WriteCacheFile(const glz::json_t& data, std::string* error) {
  const auto path = vinput::path::ProviderModelCachePath();
  std::string content;
  if (bool(glz::write<kCacheWriteOpts>(data, content))) {
    if (error != nullptr) {
      *error = "failed to serialize provider model cache";
    }
    return false;
  }
  auto res = vinput::file::AtomicWriteTextFile(path, content);
  if (!res) {
    if (error != nullptr) {
      *error = res.error();
    }
    return false;
  }
  return true;
}

} // namespace

bool SaveProviderModels(const std::string& provider_id, const std::vector<std::string>& models,
                        std::string* error) {
  if (provider_id.empty()) {
    if (error != nullptr) {
      *error = "Provider ID cannot be empty";
    }
    return false;
  }

  glz::json_t cache;
  ReadCacheFile(&cache);

  auto* providers = vinput::json::Find(cache, "providers");
  if (providers == nullptr || !providers->is_object()) {
    cache["providers"] = EmptyObject();
    providers = vinput::json::Find(cache, "providers");
  }
  if (providers == nullptr) {
    if (error != nullptr) {
      *error = "failed to prepare provider model cache";
    }
    return false;
  }
  (*providers)[provider_id] = glz::json_t(glz::json_t::object_t{
      {"models", vinput::json::FromStrings(models)},
      {"updated_at", glz::json_t(static_cast<double>(std::time(nullptr)))},
  });
  return WriteCacheFile(cache, error);
}

bool RemoveProviderModels(const std::string& provider_id, std::string* error) {
  if (provider_id.empty()) {
    return true;
  }

  glz::json_t cache;
  if (!ReadCacheFile(&cache)) {
    return true;
  }

  auto* providers = vinput::json::Find(cache, "providers");
  if (providers != nullptr && providers->is_object() &&
      providers->get_object().contains(provider_id)) {
    providers->get_object().erase(provider_id);
    return WriteCacheFile(cache, error);
  }
  return true;
}

std::vector<ModelEntry> LoadActiveModels(const CoreConfig& config) {
  std::set<std::string> active_provider_ids;
  for (const auto& provider : config.llm.providers) {
    if (!provider.id.empty()) {
      active_provider_ids.insert(provider.id);
    }
  }

  glz::json_t cache;
  const bool has_cache = ReadCacheFile(&cache);
  bool cache_modified = false;

  auto* cache_providers = has_cache ? vinput::json::Find(cache, "providers") : nullptr;
  if (cache_providers != nullptr && cache_providers->is_object()) {
    std::vector<std::string> orphaned_ids;
    for (const auto& [cached_id, cached_value] : cache_providers->get_object()) {
      if (!active_provider_ids.contains(cached_id)) {
        orphaned_ids.push_back(cached_id);
      }
    }
    for (const auto& orphan : orphaned_ids) {
      cache_providers->get_object().erase(orphan);
      cache_modified = true;
    }
    if (cache_modified) {
      std::string err;
      WriteCacheFile(cache, &err);
    }
  }

  // Determine current active model for command mode
  std::string active_cmd_provider;
  std::string active_cmd_model;
  for (const auto& scene : config.scenes.definitions) {
    if (scene.id == vinput::scene::kCommandSceneId) {
      active_cmd_provider = scene.provider_id;
      active_cmd_model = scene.model;
      break;
    }
  }

  std::vector<ModelEntry> results;
  std::set<std::string> seen_pairs;

  auto add_entry = [&](const std::string& pid, const std::string& mdl) {
    if (pid.empty() || mdl.empty()) {
      return;
    }
    const std::string key = pid + "/" + mdl;
    if (seen_pairs.insert(key).second) {
      const bool is_active = (pid == active_cmd_provider && mdl == active_cmd_model);
      results.push_back(ModelEntry{
          .provider_id = pid,
          .model = mdl,
          .full_id = key,
          .active_for_command = is_active,
      });
    }
  };

  // 1. Models from active cached providers
  if (cache_providers != nullptr && cache_providers->is_object()) {
    for (const auto& pid : active_provider_ids) {
      const auto* entry = vinput::json::Find(*cache_providers, pid);
      if (entry == nullptr) {
        continue;
      }
      for (const auto& model : vinput::json::Strings(*entry, "models")) {
        add_entry(pid, model);
      }
    }
  }

  // 2. Statically configured models from scenes (e.g. __command__ or custom scenes)
  for (const auto& scene : config.scenes.definitions) {
    if (!scene.provider_id.empty() && !scene.model.empty() &&
        active_provider_ids.contains(scene.provider_id)) {
      add_entry(scene.provider_id, scene.model);
    }
  }

  return results;
}

bool SetActiveCommandModel(CoreConfig* config, const std::string& provider_id,
                           const std::string& model, std::string* error) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "Config pointer is null";
    }
    return false;
  }

  bool found = false;
  for (auto& scene : config->scenes.definitions) {
    if (scene.id == vinput::scene::kCommandSceneId) {
      scene.provider_id = provider_id;
      scene.model = model;
      found = true;
      break;
    }
  }

  if (!found) {
    vinput::scene::Definition cmd_scene;
    cmd_scene.id = std::string(vinput::scene::kCommandSceneId);
    cmd_scene.label = std::string(vinput::scene::kCommandSceneLabelKey);
    cmd_scene.provider_id = provider_id;
    cmd_scene.model = model;
    cmd_scene.builtin = true;
    config->scenes.definitions.push_back(std::move(cmd_scene));
  }

  NormalizeCoreConfig(config);
  const bool ok = SaveCoreConfig(*config);
  if (!ok && error != nullptr) {
    *error = "Failed to save core config";
  }
  return ok;
}

} // namespace vinput::llm
