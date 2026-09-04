#include "common/llm/provider_model_cache.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/config/core_config.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/path_utils.h"

namespace vinput::llm {

namespace {

using json = nlohmann::json;

bool ReadCacheFile(json* out) {
  if (out == nullptr) {
    return false;
  }
  *out = json::object();

  const auto path = vinput::path::ProviderModelCachePath();
  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    return false;
  }

  try {
    *out = json::parse(ifs);
    return out->is_object();
  } catch (...) {
    *out = json::object();
    return false;
  }
}

bool WriteCacheFile(const json& data, std::string* error) {
  const auto path = vinput::path::ProviderModelCachePath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error != nullptr) {
      *error = "Failed to create cache directory: " + ec.message();
    }
    return false;
  }

  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  if (!ofs.is_open()) {
    if (error != nullptr) {
      *error = "Failed to write model cache file: " + path.string();
    }
    return false;
  }

  ofs << data.dump(2);
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

  json cache;
  ReadCacheFile(&cache);

  if (!cache.contains("providers") || !cache["providers"].is_object()) {
    cache["providers"] = json::object();
  }

  json prov_entry = json::object();
  prov_entry["models"] = models;
  prov_entry["updated_at"] = static_cast<long long>(std::time(nullptr));

  cache["providers"][provider_id] = std::move(prov_entry);
  return WriteCacheFile(cache, error);
}

bool RemoveProviderModels(const std::string& provider_id, std::string* error) {
  if (provider_id.empty()) {
    return true;
  }

  json cache;
  if (!ReadCacheFile(&cache)) {
    return true;
  }

  if (cache.contains("providers") && cache["providers"].is_object() &&
      cache["providers"].contains(provider_id)) {
    cache["providers"].erase(provider_id);
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

  json cache;
  const bool has_cache = ReadCacheFile(&cache);
  bool cache_modified = false;

  if (has_cache && cache.contains("providers") && cache["providers"].is_object()) {
    std::vector<std::string> orphaned_ids;
    for (auto it = cache["providers"].begin(); it != cache["providers"].end(); ++it) {
      if (active_provider_ids.find(it.key()) == active_provider_ids.end()) {
        orphaned_ids.push_back(it.key());
      }
    }
    for (const auto& orphan : orphaned_ids) {
      cache["providers"].erase(orphan);
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
  if (has_cache && cache.contains("providers") && cache["providers"].is_object()) {
    for (const auto& pid : active_provider_ids) {
      if (cache["providers"].contains(pid) && cache["providers"][pid].contains("models") &&
          cache["providers"][pid]["models"].is_array()) {
        for (const auto& m_val : cache["providers"][pid]["models"]) {
          if (m_val.is_string()) {
            add_entry(pid, m_val.get<std::string>());
          }
        }
      }
    }
  }

  // 2. Statically configured models from scenes (e.g. __command__ or custom scenes)
  for (const auto& scene : config.scenes.definitions) {
    if (!scene.provider_id.empty() && !scene.model.empty() &&
        active_provider_ids.find(scene.provider_id) != active_provider_ids.end()) {
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
