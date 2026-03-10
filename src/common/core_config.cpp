#include "core_config.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "common/file_utils.h"
#include "common/path_utils.h"

using json = nlohmann::json;

std::string GetCoreConfigPath() {
  return vinput::path::CoreConfigPath().string();
}

// ---------------------------------------------------------------------------
// LlmProvider serialization
// ---------------------------------------------------------------------------

void to_json(json &j, const LlmProvider &p) {
  j = json{{"name", p.name},
           {"base_url", p.base_url},
           {"api_key", p.api_key},
           {"model", p.model},
           {"candidate_count", p.candidate_count},
           {"timeout_ms", p.timeout_ms}};
}

void from_json(const json &j, LlmProvider &p) {
  p.name = j.value("name", p.name);
  p.base_url = j.value("base_url", p.base_url);
  p.api_key = j.value("api_key", p.api_key);
  p.model = j.value("model", p.model);
  p.candidate_count = j.value("candidate_count", p.candidate_count);
  p.timeout_ms = j.value("timeout_ms", p.timeout_ms);
}

// ---------------------------------------------------------------------------
// scene::Definition serialization (in vinput::scene namespace for ADL)
// ---------------------------------------------------------------------------

namespace vinput::scene {

void to_json(json &j, const Definition &d) {
  j = json{{"id", d.id},
           {"label", d.label},
           {"llm", d.llm},
           {"prompt", d.prompt}};
}

void from_json(const json &j, Definition &d) {
  d.id = j.value("id", std::string{});
  d.label = j.value("label", std::string{});
  d.llm = j.value("llm", false);
  d.prompt = j.value("prompt", std::string{});
}

}  // namespace vinput::scene

// ---------------------------------------------------------------------------
// CoreConfig::Llm serialization
// ---------------------------------------------------------------------------

void to_json(json &j, const CoreConfig::Llm &p) {
  j = json{{"enabled", p.enabled},
           {"active_provider", p.activeProvider},
           {"providers", p.providers},
           {"candidate_count", p.candidateCount}};
}

void from_json(const json &j, CoreConfig::Llm &p) {
  p.enabled = j.value("enabled", p.enabled);
  p.activeProvider = j.value("active_provider", p.activeProvider);
  if (j.contains("providers")) {
    p.providers = j.at("providers").get<std::vector<LlmProvider>>();
  }
  p.candidateCount = j.value("candidate_count", p.candidateCount);
}

// ---------------------------------------------------------------------------
// CoreConfig::Scenes serialization
// ---------------------------------------------------------------------------

void to_json(json &j, const CoreConfig::Scenes &s) {
  j = json::object();
  j["active_scene"] = s.activeScene;
  j["definitions"] = s.definitions;
}

void from_json(const json &j, CoreConfig::Scenes &s) {
  s.activeScene = j.value("active_scene", s.activeScene);
  if (j.contains("definitions")) {
    s.definitions =
        j.at("definitions").get<std::vector<vinput::scene::Definition>>();
  }
}

// ---------------------------------------------------------------------------
// CoreConfig serialization (top-level fields, no "core" wrapper)
// ---------------------------------------------------------------------------

void to_json(json &j, const CoreConfig &p) {
  j = json::object();
  j["capture_device"] = p.captureDevice;
  j["active_model"] = p.activeModel;
  j["model_base_dir"] = p.modelBaseDir;
  j["registry_url"] = p.registryUrl;
  j["llm"] = p.llm;
  j["default_language"] = p.defaultLanguage;
  j["hotwords"] = p.hotwords;
  j["hotwords_score"] = p.hotwordsScore;
  j["scenes"] = p.scenes;
}

void from_json(const json &j, CoreConfig &p) {
  p.captureDevice = j.value("capture_device", p.captureDevice);
  p.activeModel = j.value("active_model", p.activeModel);
  p.modelBaseDir = j.value("model_base_dir", p.modelBaseDir);
  if (auto v = j.value("registry_url", std::string{}); !v.empty()) {
    p.registryUrl = std::move(v);
  }
  if (j.contains("llm")) {
    p.llm = j.at("llm").get<CoreConfig::Llm>();
  }
  p.defaultLanguage = j.value("default_language", p.defaultLanguage);
  if (j.contains("hotwords")) {
    p.hotwords = j.at("hotwords").get<std::vector<std::string>>();
  }
  p.hotwordsScore = j.value("hotwords_score", p.hotwordsScore);
  if (j.contains("scenes")) {
    p.scenes = j.at("scenes").get<CoreConfig::Scenes>();
  }
}

// ---------------------------------------------------------------------------
// LoadCoreConfig
// ---------------------------------------------------------------------------

CoreConfig LoadCoreConfig() {
  CoreConfig config;
  std::filesystem::path path = vinput::path::CoreConfigPath();
  std::ifstream f(path);
  if (!f.is_open()) {
    return config;
  }

  try {
    json j;
    f >> j;
    config = j.get<CoreConfig>();
  } catch (const json::exception &e) {
    std::cerr << "Failed to parse vinput config: " << e.what() << std::endl;
  }
  return config;
}

// ---------------------------------------------------------------------------
// SaveCoreConfig
// ---------------------------------------------------------------------------

bool SaveCoreConfig(const CoreConfig &config) {
  std::filesystem::path path = vinput::path::CoreConfigPath();

  std::string err;
  if (!vinput::file::EnsureParentDirectory(path, &err)) {
    std::cerr << "Failed to create config directory: " << err << "\n";
    return false;
  }

  try {
    json j = config;
    std::string content = j.dump(4) + "\n";
    if (!vinput::file::AtomicWriteTextFile(path, content, &err)) {
      std::cerr << "Failed to write config: " << err << "\n";
      return false;
    }
    return true;
  } catch (const json::exception &e) {
    std::cerr << "Failed to serialize vinput config: " << e.what() << "\n";
    return false;
  }
}

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

void NormalizeCoreConfig(CoreConfig *config) {
  if (!config) return;
  if (!config->modelBaseDir.empty()) {
    config->modelBaseDir =
        vinput::path::ExpandUserPath(config->modelBaseDir).string();
  }
}

const LlmProvider *ResolveActiveLlmProvider(const CoreConfig &config) {
  const std::string &current = config.llm.activeProvider;
  for (const auto &p : config.llm.providers) {
    if (p.name == current) {
      return &p;
    }
  }
  return nullptr;
}

std::filesystem::path ResolveModelBaseDir(const CoreConfig &config) {
  if (!config.modelBaseDir.empty()) {
    return vinput::path::ExpandUserPath(config.modelBaseDir);
  }
  return vinput::path::DefaultModelBaseDir();
}
