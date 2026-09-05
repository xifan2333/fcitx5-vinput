#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vinput::scene {

constexpr int kMinLlmMaxCandidates = 0;
constexpr int kMaxLlmMaxCandidates = 9;
constexpr int kDefaultLlmMaxCandidates = 1;
constexpr int kDefaultTimeoutMs = 4000;
constexpr int kDefaultContextLines = 0;
constexpr std::string_view kRawSceneId = "__raw__";
constexpr std::string_view kCommandSceneId = "__command__";
constexpr std::string_view kRawSceneLabelKey = "__label_raw__";
constexpr std::string_view kCommandSceneLabelKey = "__label_command__";

struct Definition {
  std::string id;
  std::string label;
  std::string prompt;
  std::string provider_id;
  std::string model;
  int llm_max_candidates = kDefaultLlmMaxCandidates;
  int timeout_ms = kDefaultTimeoutMs;
  int context_lines = kDefaultContextLines;
  bool show_raw = true;
  bool builtin = false;
};

struct Config {
  std::string activeSceneId;
  std::vector<Definition> scenes;
};

int NormalizeLlmMaxCandidates(int llm_max_candidates);
bool IsBuiltinSceneId(std::string_view scene_id);
bool IsBuiltinSceneLabelKey(std::string_view label);
void NormalizeDefinition(Definition* scene);
bool ValidateDefinition(const Definition& scene, std::string* error, bool require_id = true);

const Definition* Find(const Config& config, std::string_view scene_id);
const Definition& Resolve(const Config& config, std::string_view scene_id);
std::string DisplayLabel(const Definition& scene);

bool AddScene(Config* config, const Definition& def, std::string* error);
bool UpdateScene(Config* config, const std::string& id, const Definition& def, std::string* error);
bool RemoveScene(Config* config, const std::string& id, bool force, std::string* error);
bool SetActiveScene(Config* config, const std::string& id, std::string* error);
int ClearProviderReferences(Config* config, std::string_view provider_id);

} // namespace vinput::scene
