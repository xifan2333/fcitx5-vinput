#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"

using json = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// LlmProvider
// ---------------------------------------------------------------------------

void to_json(json& j, const LlmProvider& p) {
  j = json::object();
  j["id"] = p.id;
  if (!p.base_url.empty()) {
    j["base_url"] = p.base_url;
  }
  if (!p.api_key.empty()) {
    j["api_key"] = p.api_key;
  }
  if (p.extra_body.is_object() && !p.extra_body.empty()) {
    j["extra_body"] = p.extra_body;
  }
}

void from_json(const json& j, LlmProvider& p) {
  p.id = j.value("id", p.id);
  p.base_url = j.value("base_url", p.base_url);
  p.api_key = j.value("api_key", p.api_key);
  if (j.contains("extra_body") && j.at("extra_body").is_object()) {
    p.extra_body = j.at("extra_body");
  } else {
    p.extra_body = nlohmann::json::object();
  }
}

// ---------------------------------------------------------------------------
// LlmAdapter
// ---------------------------------------------------------------------------

void to_json(json& j, const LlmAdapter& p) {
  j = json::object();
  j["id"] = p.id;
  j["command"] = p.command;
  if (!p.args.empty()) {
    j["args"] = p.args;
  }
  if (!p.env.empty()) {
    j["env"] = p.env;
  }
  if (p.autoStart) {
    j["auto_start"] = p.autoStart;
  }
}

void from_json(const json& j, LlmAdapter& p) {
  p.id = j.value("id", p.id);
  p.command = j.value("command", p.command);
  if (j.contains("args") && j.at("args").is_array()) {
    p.args = j.at("args").get<std::vector<std::string>>();
  }
  if (j.contains("env") && j.at("env").is_object()) {
    p.env = j.at("env").get<std::map<std::string, std::string>>();
  }
  p.autoStart = j.value("auto_start", p.autoStart);
}

// ---------------------------------------------------------------------------
// LocalAsrProvider
// ---------------------------------------------------------------------------

void to_json(json& j, const LocalAsrProvider& p) {
  j = json::object();
  j["id"] = p.id;
  j["type"] = vinput::asr::kLocalProviderType;
  if (!p.model.empty()) {
    j["model"] = p.model;
  }
  if (!p.hotwordsFile.empty()) {
    j["hotwords_file"] = p.hotwordsFile;
  }
  if (p.timeoutMs > 0) {
    j["timeout_ms"] = p.timeoutMs;
  }
}

void from_json(const json& j, LocalAsrProvider& p) {
  p.id = j.value("id", p.id);
  p.model = j.value("model", p.model);
  p.hotwordsFile = j.value("hotwords_file", p.hotwordsFile);
  p.timeoutMs = j.value("timeout_ms", p.timeoutMs);
}

// ---------------------------------------------------------------------------
// CommandAsrProvider
// ---------------------------------------------------------------------------

void to_json(json& j, const CommandAsrProvider& p) {
  j = json::object();
  j["id"] = p.id;
  j["type"] = vinput::asr::kCommandProviderType;
  if (!p.command.empty()) {
    j["command"] = p.command;
  }
  if (!p.args.empty()) {
    j["args"] = p.args;
  }
  if (!p.env.empty()) {
    j["env"] = p.env;
  }
  if (p.timeoutMs > 0) {
    j["timeout_ms"] = p.timeoutMs;
  }
}

void from_json(const json& j, CommandAsrProvider& p) {
  p.id = j.value("id", p.id);
  p.command = j.value("command", p.command);
  if (j.contains("args") && j.at("args").is_array()) {
    p.args = j.at("args").get<std::vector<std::string>>();
  }
  if (j.contains("env") && j.at("env").is_object()) {
    p.env = j.at("env").get<std::map<std::string, std::string>>();
  }
  p.timeoutMs = j.value("timeout_ms", p.timeoutMs);
}

// ---------------------------------------------------------------------------
// AsrProvider (variant)
// ---------------------------------------------------------------------------

void to_json(json& j, const AsrProvider& p) {
  std::visit([&j](const auto& provider) { to_json(j, provider); }, p);
}

void from_json(const json& j, AsrProvider& p) {
  const std::string type = j.value("type", std::string{});
  if (type == vinput::asr::kLocalProviderType) {
    LocalAsrProvider local;
    from_json(j, local);
    p = std::move(local);
  } else if (type == vinput::asr::kCommandProviderType) {
    CommandAsrProvider cmd;
    from_json(j, cmd);
    p = std::move(cmd);
  }
  // unknown type: leave p in default-constructed state
}

// ---------------------------------------------------------------------------
// CoreConfig::Global
// ---------------------------------------------------------------------------

void to_json(json& j, const CoreConfig::Global& g) {
  j = json::object();
  j["default_language"] = g.defaultLanguage;
  j["capture_device"] = g.captureDevice;
  j["duck_output_while_recording"] = g.duckOutputWhileRecording;
  j["duck_output_volume"] = g.duckOutputVolume;
}

void from_json(const json& j, CoreConfig::Global& g) {
  g.defaultLanguage = j.value("default_language", g.defaultLanguage);
  g.captureDevice = j.value("capture_device", g.captureDevice);
  g.duckOutputWhileRecording = j.value("duck_output_while_recording", g.duckOutputWhileRecording);
  g.duckOutputVolume = j.value("duck_output_volume", g.duckOutputVolume);
}

// ---------------------------------------------------------------------------
// vinput::scene::Definition
// ---------------------------------------------------------------------------

namespace vinput::scene {

void to_json(json& j, const Definition& d) {
  j = json::object();
  j["id"] = d.id;
  if (!d.label.empty() && !IsBuiltinSceneLabelKey(d.label) && !IsBuiltinSceneId(d.id)) {
    j["label"] = d.label;
  }
  if (!d.prompt.empty()) {
    j["prompt"] = d.prompt;
  }
  if (!d.provider_id.empty()) {
    j["provider_id"] = d.provider_id;
  }
  if (!d.model.empty()) {
    j["model"] = d.model;
  }
  if (d.llm_max_candidates != vinput::scene::kDefaultLlmMaxCandidates) {
    j["llm_max_candidates"] = d.llm_max_candidates;
  }
  if (d.timeout_ms != vinput::scene::kDefaultTimeoutMs) {
    j["timeout_ms"] = d.timeout_ms;
  }
  if (d.context_lines != vinput::scene::kDefaultContextLines) {
    j["context_lines"] = d.context_lines;
  }
}

void from_json(const json& j, Definition& d) {
  d.id = j.value("id", std::string{});
  d.label = j.value("label", std::string{});
  d.prompt = j.value("prompt", std::string{});
  d.provider_id = j.value("provider_id", std::string{});
  d.model = j.value("model", std::string{});
  if (j.contains("llm_max_candidates")) {
    d.llm_max_candidates = j.value("llm_max_candidates", vinput::scene::kDefaultLlmMaxCandidates);
  } else {
    // Backward compatibility for configurations written before v2.3.14.
    d.llm_max_candidates = j.value("candidate_count", vinput::scene::kDefaultLlmMaxCandidates);
  }
  d.timeout_ms = j.value("timeout_ms", vinput::scene::kDefaultTimeoutMs);
  d.context_lines = j.value("context_lines", vinput::scene::kDefaultContextLines);
}

} // namespace vinput::scene

// ---------------------------------------------------------------------------
// CoreConfig::Llm
// ---------------------------------------------------------------------------

void to_json(json& j, const CoreConfig::Llm& p) {
  j = json::object();
  j["providers"] = p.providers;
  j["adapters"] = p.adapters;
}

void from_json(const json& j, CoreConfig::Llm& p) {
  if (j.contains("providers")) {
    p.providers = j.at("providers").get<std::vector<LlmProvider>>();
  }
  if (j.contains("adapters")) {
    p.adapters = j.at("adapters").get<std::vector<LlmAdapter>>();
  }
}

// ---------------------------------------------------------------------------
// CoreConfig::Asr
// ---------------------------------------------------------------------------

void to_json(json& j, const CoreConfig::Asr::Vad& v) {
  j = json::object();
  j["enabled"] = v.enabled;
  j["threshold"] = v.threshold;
  j["min_speech_duration"] = v.minSpeechDuration;
  j["min_silence_duration"] = v.minSilenceDuration;
  j["speech_pad_ms"] = v.speechPadMs;
}

void from_json(const json& j, CoreConfig::Asr::Vad& v) {
  v.enabled = j.value("enabled", v.enabled);
  v.threshold = j.value("threshold", v.threshold);
  v.minSpeechDuration = j.value("min_speech_duration", v.minSpeechDuration);
  v.minSilenceDuration = j.value("min_silence_duration", v.minSilenceDuration);
  v.speechPadMs = j.value("speech_pad_ms", v.speechPadMs);
}

void to_json(json& j, const CoreConfig::Asr& a) {
  j = json::object();
  j["active_provider"] = a.activeProvider;
  j["normalize_audio"] = a.normalizeAudio;
  j["input_gain"] = a.inputGain;
  j["vad"] = a.vad;
  j["providers"] = a.providers;
}

void from_json(const json& j, CoreConfig::Asr& a) {
  a.activeProvider = j.value("active_provider", a.activeProvider);
  a.normalizeAudio = j.value("normalize_audio", a.normalizeAudio);
  a.inputGain = j.value("input_gain", a.inputGain);
  if (j.contains("vad")) {
    a.vad = j.at("vad").get<CoreConfig::Asr::Vad>();
  }
  if (j.contains("providers")) {
    a.providers = j.at("providers").get<std::vector<AsrProvider>>();
  }
}

// ---------------------------------------------------------------------------
// CoreConfig::Registry
// ---------------------------------------------------------------------------

void to_json(json& j, const CoreConfig::Registry& r) {
  j = json::object();
  if (!r.baseUrls.empty()) {
    j["base_urls"] = r.baseUrls;
  }
}

void from_json(const json& j, CoreConfig::Registry& r) {
  r.baseUrls.clear();
  if (j.contains("base_urls") && j.at("base_urls").is_array()) {
    for (const auto& value : j.at("base_urls")) {
      if (value.is_string() && !value.get<std::string>().empty()) {
        r.baseUrls.push_back(value.get<std::string>());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// CoreConfig::Scenes
// ---------------------------------------------------------------------------

void to_json(json& j, const CoreConfig::Scenes& s) {
  j = json::object();
  j["active_scene"] = s.activeScene;
  j["definitions"] = s.definitions;
}

void from_json(const json& j, CoreConfig::Scenes& s) {
  s.activeScene = j.value("active_scene", s.activeScene);
  if (j.contains("definitions")) {
    s.definitions = j.at("definitions").get<std::vector<vinput::scene::Definition>>();
  }
}

// ---------------------------------------------------------------------------
// CoreConfig (top-level)
// ---------------------------------------------------------------------------

void to_json(json& j, const CoreConfig& p) {
  j = json::object();
  j["version"] = p.version;
  j["registry"] = p.registry;
  j["global"] = p.global;
  j["asr"] = p.asr;
  j["llm"] = p.llm;
  j["scenes"] = p.scenes;
}

void from_json(const json& j, CoreConfig& p) {
  p.version = j.value("version", p.version);
  if (j.contains("registry")) {
    p.registry = j.at("registry").get<CoreConfig::Registry>();
  }
  if (j.contains("global")) {
    p.global = j.at("global").get<CoreConfig::Global>();
  }
  if (j.contains("llm")) {
    p.llm = j.at("llm").get<CoreConfig::Llm>();
  }
  if (j.contains("scenes")) {
    p.scenes = j.at("scenes").get<CoreConfig::Scenes>();
  }
  if (j.contains("asr")) {
    p.asr = j.at("asr").get<CoreConfig::Asr>();
  }
}
