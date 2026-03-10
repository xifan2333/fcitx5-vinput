#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/postprocess_scene.h"

struct LlmProvider {
  std::string name;
  std::string base_url;
  std::string api_key;
  std::string model;
  int candidate_count = 1;
  int timeout_ms = 4000;
};

struct CoreConfig {
  std::string captureDevice{"default"};
  std::string activeModel{"paraformer-zh"};
  std::string modelBaseDir;
  std::string registryUrl{"https://raw.githubusercontent.com/xifan2333/vinput-models/main/registry.json"};

  std::string defaultLanguage{"zh"};

  std::vector<std::string> hotwords;
  float hotwordsScore{1.5f};

  struct Llm {
    bool enabled{false};
    std::string activeProvider;
    std::vector<LlmProvider> providers;
    int candidateCount{1};  // global default
  } llm;

  struct Scenes {
    std::string activeScene{"default"};
    std::vector<vinput::scene::Definition> definitions;
  } scenes;
};

// API Functions
CoreConfig LoadCoreConfig();
bool SaveCoreConfig(const CoreConfig &config);
std::string GetCoreConfigPath();

void NormalizeCoreConfig(CoreConfig *config);
const LlmProvider *ResolveActiveLlmProvider(const CoreConfig &config);
std::filesystem::path ResolveModelBaseDir(const CoreConfig &config);
