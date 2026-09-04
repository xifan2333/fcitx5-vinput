#pragma once

#include <string>
#include <vector>

struct CoreConfig;

namespace vinput::llm {

struct ModelEntry {
  std::string provider_id;
  std::string model;
  std::string full_id; // "provider_id/model"
  bool active_for_command = false;
};

// Saves or updates the cached model list for a specific provider.
bool SaveProviderModels(const std::string& provider_id, const std::vector<std::string>& models,
                        std::string* error = nullptr);

// Removes the cached model list for a specific provider.
bool RemoveProviderModels(const std::string& provider_id, std::string* error = nullptr);

// Loads active models across all currently configured providers, purging orphaned providers
// from the cache. Also includes any statically configured scene models.
std::vector<ModelEntry> LoadActiveModels(const CoreConfig& config);

// Updates the active provider/model binding for the command mode scene (__command__).
bool SetActiveCommandModel(CoreConfig* config, const std::string& provider_id,
                           const std::string& model, std::string* error = nullptr);

} // namespace vinput::llm
