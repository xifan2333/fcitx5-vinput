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
// CoreConfig::Core serialization
// ---------------------------------------------------------------------------

void to_json(json &j, const CoreConfig::Core &p) {
  j = json{{"capture_device", p.captureDevice},
           {"active_model", p.activeModel},
           {"model_base_dir", p.modelBaseDir},
           {"registry_url", p.registryUrl}};
}

void from_json(const json &j, CoreConfig::Core &p) {
  p.captureDevice = j.value("capture_device", p.captureDevice);
  p.activeModel = j.value("active_model", p.activeModel);
  p.modelBaseDir = j.value("model_base_dir", p.modelBaseDir);
  p.registryUrl = j.value("registry_url", p.registryUrl);
}

// ---------------------------------------------------------------------------
// CoreConfig::Llm serialization (new multi-provider format)
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
// CoreConfig::Ui serialization
// ---------------------------------------------------------------------------

void to_json(json &j, const CoreConfig::Ui &p) {
  j = json{{"language", p.language}, {"theme", p.theme}};
}

void from_json(const json &j, CoreConfig::Ui &p) {
  p.language = j.value("language", p.language);
  p.theme = j.value("theme", p.theme);
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
// CoreConfig serialization
// ---------------------------------------------------------------------------

void to_json(json &j, const CoreConfig &p) {
  j = json::object();
  j["core"] = p.core;
  j["llm"] = p.llm;
  j["ui"] = p.ui;
  j["scenes"] = p.scenes;
}

void from_json(const json &j, CoreConfig &p) {
  if (j.contains("core")) {
    p.core = j.at("core").get<CoreConfig::Core>();
  }
  if (j.contains("llm")) {
    p.llm = j.at("llm").get<CoreConfig::Llm>();
  }
  if (j.contains("ui")) {
    p.ui = j.at("ui").get<CoreConfig::Ui>();
  }
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
  // Expand ~ in modelBaseDir
  if (!config->core.modelBaseDir.empty()) {
    config->core.modelBaseDir =
        vinput::path::ExpandUserPath(config->core.modelBaseDir).string();
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

std::filesystem::path ResolveModelBaseDir(const CoreConfig::Core &core) {
  if (!core.modelBaseDir.empty()) {
    return vinput::path::ExpandUserPath(core.modelBaseDir);
  }
  return vinput::path::DefaultModelBaseDir();
}
