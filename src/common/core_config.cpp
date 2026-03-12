#include "core_config.h"

#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <system_error>

#include "common/file_utils.h"
#include "common/path_utils.h"

using json = nlohmann::json;

namespace {

struct ConfigCache {
  std::mutex mu;
  bool has_cache = false;
  std::filesystem::file_time_type mtime;
  std::uintmax_t size = 0;
  CoreConfig cached;
};

ConfigCache &GetConfigCache() {
  static ConfigCache cache;
  return cache;
}

bool GetConfigStat(const std::filesystem::path &path,
                   std::filesystem::file_time_type *mtime,
                   std::uintmax_t *size) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return false;
  }
  auto t = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return false;
  }
  auto s = std::filesystem::file_size(path, ec);
  if (ec) {
    return false;
  }
  if (mtime) *mtime = t;
  if (size) *size = s;
  return true;
}

}  // namespace

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
           {"timeout_ms", p.timeout_ms}};
}

void from_json(const json &j, LlmProvider &p) {
  p.name = j.value("name", p.name);
  p.base_url = j.value("base_url", p.base_url);
  p.api_key = j.value("api_key", p.api_key);
  p.model = j.value("model", p.model);
  p.timeout_ms = j.value("timeout_ms", p.timeout_ms);
}

// ---------------------------------------------------------------------------
// scene::Definition serialization (in vinput::scene namespace for ADL)
// ---------------------------------------------------------------------------

namespace vinput::scene {

void to_json(json &j, const Definition &d) {
  j = json{{"id", d.id},
           {"label", d.label},
           {"prompt", d.prompt}};
}

void from_json(const json &j, Definition &d) {
  d.id = j.value("id", std::string{});
  d.label = j.value("label", std::string{});
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
           {"candidate_count", p.candidateCount},
           {"command_candidate_count", p.commandCandidateCount}};
}

void from_json(const json &j, CoreConfig::Llm &p) {
  p.enabled = j.value("enabled", p.enabled);
  p.activeProvider = j.value("active_provider", p.activeProvider);
  if (j.contains("providers")) {
    p.providers = j.at("providers").get<std::vector<LlmProvider>>();
  }
  p.candidateCount = j.value("candidate_count", p.candidateCount);
  p.commandCandidateCount = j.value("command_candidate_count", p.commandCandidateCount);
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
  j["hotwords_file"] = p.hotwordsFile;
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
  p.hotwordsFile = j.value("hotwords_file", p.hotwordsFile);
  if (j.contains("scenes")) {
    p.scenes = j.at("scenes").get<CoreConfig::Scenes>();
  }
}

namespace {

CoreConfig LoadCoreConfigFromFile(const std::filesystem::path &path) {
  CoreConfig config;
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

}  // namespace

// ---------------------------------------------------------------------------
// LoadCoreConfig
// ---------------------------------------------------------------------------

CoreConfig LoadCoreConfig() {
  std::filesystem::path path = vinput::path::CoreConfigPath();
  std::filesystem::file_time_type mtime;
  std::uintmax_t size = 0;
  const bool stat_ok = GetConfigStat(path, &mtime, &size);

  auto &cache = GetConfigCache();
  {
    std::lock_guard<std::mutex> lock(cache.mu);
    if (stat_ok && cache.has_cache && cache.mtime == mtime &&
        cache.size == size) {
      return cache.cached;
    }
  }

  if (!stat_ok) {
    return CoreConfig{};
  }

  CoreConfig config = LoadCoreConfigFromFile(path);
  {
    std::lock_guard<std::mutex> lock(cache.mu);
    cache.cached = config;
    cache.mtime = mtime;
    cache.size = size;
    cache.has_cache = true;
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
