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
  int timeout_ms = 4000;
};

struct CoreConfig {
  std::string captureDevice{"default"};
  std::string activeModel{"paraformer-zh"};
  std::string modelBaseDir;
  std::string registryUrl{"https://raw.githubusercontent.com/xifan2333/vinput-models/main/registry.json"};

  std::string defaultLanguage{"zh"};

  std::string hotwordsFile;

  struct Llm {
    bool enabled{false};
    std::string activeProvider;
    std::vector<LlmProvider> providers;
    int candidateCount{1};         // postprocess candidate count
    int commandCandidateCount{1};  // command mode candidate count
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
