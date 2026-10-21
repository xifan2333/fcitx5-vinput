#include <cstdint>
#include <glaze/core/common.hpp>
#include <glaze/core/meta.hpp>
#include <glaze/core/opts.hpp>
#include <glaze/core/read.hpp>
#include <glaze/core/write.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
#include "common/scene/postprocess_scene.h"

namespace {

// The on-disk `config.json` is a hand-editable file: unknown keys are ignored
// instead of aborting the parse.
constexpr auto kConfigReadOpts = glz::opts{.error_on_unknown_keys = uint8_t{0}};
// `config.json` is written with a four space indentation.
constexpr auto kConfigWriteOpts = glz::opts{.prettify = uint8_t{1}, .indentation_width = 4};

// Optional wire members are omitted when absent, which reproduces the previous
// "only write meaningful fields" behaviour of the configuration writer.
struct LlmProviderWire {
  std::string id;
  std::optional<std::string> base_url;
  std::optional<std::string> api_key;
  std::optional<glz::json_t> extra_body;
};

struct LlmAdapterWire {
  std::string id;
  std::string command;
  std::optional<glz::json_t> args;
  std::optional<glz::json_t> env;
  std::optional<bool> auto_start;
};

// A single wire shape covers both provider kinds. Member order is chosen so that
// each kind keeps its historical key order once absent members are omitted.
struct AsrProviderWire {
  std::string id;
  std::string type;
  std::optional<std::string> model;
  std::optional<std::string> hotwords_file;
  std::optional<std::string> command;
  std::optional<glz::json_t> args;
  std::optional<glz::json_t> env;
  std::optional<int> timeout_ms;
};

struct GlobalWire {
  std::string capture_device;
  bool duck_output_while_recording{false};
  double duck_output_volume{0.25};
};

struct VadWire {
  bool enabled{true};
  double threshold{0.35};
  double min_speech_duration{0.10};
  double min_silence_duration{1.2};
  int speech_pad_ms{500};
};

struct RegistryWire {
  std::optional<glz::json_t> base_urls;
};

struct SceneWire {
  std::string id;
  std::optional<std::string> label;
  std::optional<std::string> prompt;
  std::optional<std::string> provider_id;
  std::optional<std::string> model;
  std::optional<int> count;
  std::optional<int> timeout_ms;
  std::optional<int> context_lines;
  std::optional<bool> raw_cand;
  std::optional<bool> raw_prev;
};

struct LlmWire {
  std::optional<std::vector<LlmProviderWire>> providers;
  std::optional<std::vector<LlmAdapterWire>> adapters;
};

struct AsrWire {
  std::string active_provider;
  bool normalize_audio{true};
  double input_gain{1.0};
  std::optional<VadWire> vad;
  std::optional<std::vector<AsrProviderWire>> providers;
};

struct ScenesWire {
  std::string active_scene;
  std::optional<std::vector<SceneWire>> definitions;
};

struct CoreConfigWire {
  std::optional<glz::json_t> version;
  std::optional<RegistryWire> registry;
  std::optional<GlobalWire> global;
  std::optional<AsrWire> asr;
  std::optional<LlmWire> llm;
  std::optional<ScenesWire> scenes;
};

// ---------------------------------------------------------------------------
// Wire -> domain
// ---------------------------------------------------------------------------

void FromWire(const LlmProviderWire& wire, LlmProvider* provider) {
  provider->id = wire.id;
  provider->base_url = wire.base_url.value_or(provider->base_url);
  provider->api_key = wire.api_key.value_or(provider->api_key);

  provider->extra_body = glz::json_t(glz::json_t::object_t{});
  if (wire.extra_body.has_value() && wire.extra_body->is_object()) {
    provider->extra_body = *wire.extra_body;
  }
}

void FromWire(const LlmAdapterWire& wire, LlmAdapter* adapter) {
  adapter->id = wire.id;
  adapter->command = wire.command;
  if (wire.args.has_value()) {
    adapter->args = vinput::json::Strings(*wire.args);
  }
  if (wire.env.has_value()) {
    adapter->env = vinput::json::StringMap(*wire.env);
  }
  adapter->autoStart = wire.auto_start.value_or(adapter->autoStart);
}

void FromWire(const AsrProviderWire& wire, AsrProvider* provider) {
  if (wire.type == vinput::asr::kLocalProviderType) {
    LocalAsrProvider local;
    local.id = wire.id;
    local.model = wire.model.value_or(local.model);
    local.hotwordsFile = wire.hotwords_file.value_or(local.hotwordsFile);
    local.timeoutMs = wire.timeout_ms.value_or(local.timeoutMs);
    *provider = std::move(local);
    return;
  }
  if (wire.type == vinput::asr::kCommandProviderType) {
    CommandAsrProvider command;
    command.id = wire.id;
    command.command = wire.command.value_or(command.command);
    if (wire.args.has_value()) {
      command.args = vinput::json::Strings(*wire.args);
    }
    if (wire.env.has_value()) {
      command.env = vinput::json::StringMap(*wire.env);
    }
    command.timeoutMs = wire.timeout_ms.value_or(command.timeoutMs);
    *provider = std::move(command);
  }
  // Unknown provider kinds keep the default-constructed provider.
}

void FromWire(const GlobalWire& wire, CoreConfig::Global* global) {
  global->captureDevice = wire.capture_device;
  global->duckOutputWhileRecording = wire.duck_output_while_recording;
  global->duckOutputVolume = wire.duck_output_volume;
}

void FromWire(const VadWire& wire, CoreConfig::Asr::Vad* vad) {
  vad->enabled = wire.enabled;
  vad->threshold = wire.threshold;
  vad->minSpeechDuration = wire.min_speech_duration;
  vad->minSilenceDuration = wire.min_silence_duration;
  vad->speechPadMs = wire.speech_pad_ms;
}

void FromWire(const RegistryWire& wire, CoreConfig::Registry* registry) {
  registry->baseUrls.clear();
  if (!wire.base_urls.has_value()) {
    return;
  }
  for (auto& url : vinput::json::Strings(*wire.base_urls)) {
    if (!url.empty()) {
      registry->baseUrls.push_back(std::move(url));
    }
  }
}

void FromWire(const SceneWire& wire, vinput::scene::Definition* scene) {
  scene->id = wire.id;
  scene->label = wire.label.value_or(std::string{});
  scene->prompt = wire.prompt.value_or(std::string{});
  scene->provider_id = wire.provider_id.value_or(std::string{});
  scene->model = wire.model.value_or(std::string{});
  scene->llm_max_candidates = wire.count.value_or(vinput::scene::kDefaultLlmMaxCandidates);
  scene->timeout_ms = wire.timeout_ms.value_or(vinput::scene::kDefaultTimeoutMs);
  scene->context_lines = wire.context_lines.value_or(vinput::scene::kDefaultContextLines);
  scene->raw_cand = wire.raw_cand.value_or(true);
  scene->raw_prev = wire.raw_prev.value_or(true);
}

void FromWire(const LlmWire& wire, CoreConfig::Llm* llm) {
  if (wire.providers.has_value()) {
    llm->providers.clear();
    llm->providers.reserve(wire.providers->size());
    for (const auto& entry : *wire.providers) {
      LlmProvider provider;
      FromWire(entry, &provider);
      llm->providers.push_back(std::move(provider));
    }
  }
  if (wire.adapters.has_value()) {
    llm->adapters.clear();
    llm->adapters.reserve(wire.adapters->size());
    for (const auto& entry : *wire.adapters) {
      LlmAdapter adapter;
      FromWire(entry, &adapter);
      llm->adapters.push_back(std::move(adapter));
    }
  }
}

void FromWire(const AsrWire& wire, CoreConfig::Asr* asr) {
  asr->activeProvider = wire.active_provider;
  asr->normalizeAudio = wire.normalize_audio;
  asr->inputGain = wire.input_gain;
  if (wire.vad.has_value()) {
    FromWire(*wire.vad, &asr->vad);
  }
  if (wire.providers.has_value()) {
    asr->providers.clear();
    asr->providers.reserve(wire.providers->size());
    for (const auto& entry : *wire.providers) {
      AsrProvider provider = LocalAsrProvider{};
      FromWire(entry, &provider);
      asr->providers.push_back(std::move(provider));
    }
  }
}

void FromWire(const ScenesWire& wire, CoreConfig::Scenes* scenes) {
  scenes->activeScene = wire.active_scene;
  if (wire.definitions.has_value()) {
    scenes->definitions.clear();
    scenes->definitions.reserve(wire.definitions->size());
    for (const auto& entry : *wire.definitions) {
      vinput::scene::Definition definition;
      FromWire(entry, &definition);
      scenes->definitions.push_back(std::move(definition));
    }
  }
}

// ---------------------------------------------------------------------------
// Domain -> wire
// ---------------------------------------------------------------------------

LlmProviderWire ToWire(const LlmProvider& provider) {
  LlmProviderWire wire;
  wire.id = provider.id;
  if (!provider.base_url.empty()) {
    wire.base_url = provider.base_url;
  }
  if (!provider.api_key.empty()) {
    wire.api_key = provider.api_key;
  }
  if (provider.extra_body.is_object() && !provider.extra_body.empty()) {
    wire.extra_body = provider.extra_body;
  }
  return wire;
}

LlmAdapterWire ToWire(const LlmAdapter& adapter) {
  LlmAdapterWire wire;
  wire.id = adapter.id;
  wire.command = adapter.command;
  if (!adapter.args.empty()) {
    wire.args = vinput::json::FromStrings(adapter.args);
  }
  if (!adapter.env.empty()) {
    wire.env = vinput::json::FromStringMap(adapter.env);
  }
  if (adapter.autoStart) {
    wire.auto_start = true;
  }
  return wire;
}

AsrProviderWire ToWire(const AsrProvider& provider) {
  AsrProviderWire wire;
  wire.id = AsrProviderId(provider);
  wire.type = std::string(AsrProviderType(provider));

  if (const auto* local = std::get_if<LocalAsrProvider>(&provider)) {
    if (!local->model.empty()) {
      wire.model = local->model;
    }
    if (!local->hotwordsFile.empty()) {
      wire.hotwords_file = local->hotwordsFile;
    }
    if (local->timeoutMs > 0) {
      wire.timeout_ms = local->timeoutMs;
    }
    return wire;
  }

  const auto* command = std::get_if<CommandAsrProvider>(&provider);
  if (command == nullptr) {
    return wire;
  }
  if (!command->command.empty()) {
    wire.command = command->command;
  }
  if (!command->args.empty()) {
    wire.args = vinput::json::FromStrings(command->args);
  }
  if (!command->env.empty()) {
    wire.env = vinput::json::FromStringMap(command->env);
  }
  if (command->timeoutMs > 0) {
    wire.timeout_ms = command->timeoutMs;
  }
  return wire;
}

GlobalWire ToWire(const CoreConfig::Global& global) {
  GlobalWire wire;
  wire.capture_device = global.captureDevice;
  wire.duck_output_while_recording = global.duckOutputWhileRecording;
  wire.duck_output_volume = global.duckOutputVolume;
  return wire;
}

VadWire ToWire(const CoreConfig::Asr::Vad& vad) {
  VadWire wire;
  wire.enabled = vad.enabled;
  wire.threshold = vad.threshold;
  wire.min_speech_duration = vad.minSpeechDuration;
  wire.min_silence_duration = vad.minSilenceDuration;
  wire.speech_pad_ms = vad.speechPadMs;
  return wire;
}

RegistryWire ToWire(const CoreConfig::Registry& registry) {
  RegistryWire wire;
  if (!registry.baseUrls.empty()) {
    wire.base_urls = vinput::json::FromStrings(registry.baseUrls);
  }
  return wire;
}

SceneWire ToWire(const vinput::scene::Definition& scene) {
  SceneWire wire;
  wire.id = scene.id;
  if (!scene.label.empty() && !vinput::scene::IsBuiltinSceneLabelKey(scene.label) &&
      !vinput::scene::IsBuiltinSceneId(scene.id)) {
    wire.label = scene.label;
  }
  if (!scene.prompt.empty()) {
    wire.prompt = scene.prompt;
  }
  if (!scene.provider_id.empty()) {
    wire.provider_id = scene.provider_id;
  }
  if (!scene.model.empty()) {
    wire.model = scene.model;
  }
  if (scene.llm_max_candidates != vinput::scene::kDefaultLlmMaxCandidates) {
    wire.count = scene.llm_max_candidates;
  }
  if (scene.timeout_ms != vinput::scene::kDefaultTimeoutMs) {
    wire.timeout_ms = scene.timeout_ms;
  }
  if (scene.context_lines != vinput::scene::kDefaultContextLines) {
    wire.context_lines = scene.context_lines;
  }
  if (!scene.raw_cand) {
    wire.raw_cand = false;
  }
  if (!scene.raw_prev) {
    wire.raw_prev = false;
  }
  return wire;
}

LlmWire ToWire(const CoreConfig::Llm& llm) {
  LlmWire wire;
  wire.providers.emplace();
  wire.providers->reserve(llm.providers.size());
  for (const auto& provider : llm.providers) {
    wire.providers->push_back(ToWire(provider));
  }
  wire.adapters.emplace();
  wire.adapters->reserve(llm.adapters.size());
  for (const auto& adapter : llm.adapters) {
    wire.adapters->push_back(ToWire(adapter));
  }
  return wire;
}

AsrWire ToWire(const CoreConfig::Asr& asr) {
  AsrWire wire;
  wire.active_provider = asr.activeProvider;
  wire.normalize_audio = asr.normalizeAudio;
  wire.input_gain = asr.inputGain;
  wire.vad = ToWire(asr.vad);
  wire.providers.emplace();
  wire.providers->reserve(asr.providers.size());
  for (const auto& provider : asr.providers) {
    wire.providers->push_back(ToWire(provider));
  }
  return wire;
}

ScenesWire ToWire(const CoreConfig::Scenes& scenes) {
  ScenesWire wire;
  wire.active_scene = scenes.activeScene;
  wire.definitions.emplace();
  wire.definitions->reserve(scenes.definitions.size());
  for (const auto& scene : scenes.definitions) {
    wire.definitions->push_back(ToWire(scene));
  }
  return wire;
}

CoreConfigWire ToWire(const CoreConfig& config) {
  CoreConfigWire wire;
  wire.version = glz::json_t(config.version);
  wire.registry = ToWire(config.registry);
  wire.global = ToWire(config.global);
  wire.asr = ToWire(config.asr);
  wire.llm = ToWire(config.llm);
  wire.scenes = ToWire(config.scenes);
  return wire;
}

} // namespace

template <> struct glz::meta<LlmProviderWire> {
  using T = LlmProviderWire;
  static constexpr auto value = object("id", &T::id, "base_url", &T::base_url, "api_key",
                                       &T::api_key, "extra_body", &T::extra_body);
};

template <> struct glz::meta<LlmAdapterWire> {
  using T = LlmAdapterWire;
  static constexpr auto value = object("id", &T::id, "command", &T::command, "args", &T::args,
                                       "env", &T::env, "auto_start", &T::auto_start);
};

template <> struct glz::meta<AsrProviderWire> {
  using T = AsrProviderWire;
  static constexpr auto value = object(
      "id", &T::id, "type", &T::type, "model", &T::model, "hotwords_file", &T::hotwords_file,
      "command", &T::command, "args", &T::args, "env", &T::env, "timeout_ms", &T::timeout_ms);
};

template <> struct glz::meta<GlobalWire> {
  using T = GlobalWire;
  static constexpr auto value =
      object("capture_device", &T::capture_device, "duck_output_while_recording",
             &T::duck_output_while_recording, "duck_output_volume", &T::duck_output_volume);
};

template <> struct glz::meta<VadWire> {
  using T = VadWire;
  static constexpr auto value =
      object("enabled", &T::enabled, "threshold", &T::threshold, "min_speech_duration",
             &T::min_speech_duration, "min_silence_duration", &T::min_silence_duration,
             "speech_pad_ms", &T::speech_pad_ms);
};

template <> struct glz::meta<RegistryWire> {
  using T = RegistryWire;
  static constexpr auto value = object("base_urls", &T::base_urls);
};

template <> struct glz::meta<SceneWire> {
  using T = SceneWire;
  static constexpr auto value =
      object("id", &T::id, "label", &T::label, "prompt", &T::prompt, "provider_id", &T::provider_id,
             "model", &T::model, "count", &T::count, "timeout_ms", &T::timeout_ms, "context_lines",
             &T::context_lines, "raw_cand", &T::raw_cand, "raw_prev", &T::raw_prev);
};

template <> struct glz::meta<LlmWire> {
  using T = LlmWire;
  static constexpr auto value = object("providers", &T::providers, "adapters", &T::adapters);
};

template <> struct glz::meta<AsrWire> {
  using T = AsrWire;
  static constexpr auto value =
      object("active_provider", &T::active_provider, "normalize_audio", &T::normalize_audio,
             "input_gain", &T::input_gain, "vad", &T::vad, "providers", &T::providers);
};

template <> struct glz::meta<ScenesWire> {
  using T = ScenesWire;
  static constexpr auto value =
      object("active_scene", &T::active_scene, "definitions", &T::definitions);
};

template <> struct glz::meta<CoreConfigWire> {
  using T = CoreConfigWire;
  static constexpr auto value =
      object("version", &T::version, "registry", &T::registry, "global", &T::global, "asr", &T::asr,
             "llm", &T::llm, "scenes", &T::scenes);
};

std::string CoreConfigToJson(const CoreConfig& config) {
  std::string out;
  if (bool(glz::write<kConfigWriteOpts>(ToWire(config), out))) {
    return {};
  }
  return out;
}

namespace {

// Reads one top level section into its wire representation. A section that is
// missing or carries a mistyped field is skipped instead of discarding the whole
// configuration, so one bad value never resets unrelated user settings.
template <typename T> bool ReadSection(const glz::json_t& root, std::string_view key, T* out) {
  const auto* node = vinput::json::Find(root, key);
  if (node == nullptr) {
    return false;
  }
  std::string section_text;
  if (bool(glz::write_json(*node, section_text))) {
    return false;
  }
  return !bool(glz::read<kConfigReadOpts>(*out, section_text));
}

} // namespace

bool CoreConfigFromJson(std::string_view text, CoreConfig* config, std::string* error) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "Config is null.";
    }
    return false;
  }
  if (text.empty()) {
    if (error != nullptr) {
      *error = "Config is empty.";
    }
    return false;
  }

  glz::json_t root;
  if (const auto ec = glz::read_json(root, text); bool(ec)) {
    if (error != nullptr) {
      *error = glz::format_error(ec, text);
    }
    return false;
  }
  if (!root.is_object()) {
    if (error != nullptr) {
      *error = "Config root is not a JSON object.";
    }
    return false;
  }

  CoreConfig parsed;
  if (const auto* version = vinput::json::Find(root, "version"); version != nullptr) {
    if (const auto* version_text = version->get_if<std::string>()) {
      parsed.version = *version_text;
    } else if (const auto* numeric = version->get_if<double>()) {
      parsed.version = std::to_string(static_cast<int>(*numeric));
    }
  }

  RegistryWire registry;
  if (ReadSection(root, "registry", &registry)) {
    FromWire(registry, &parsed.registry);
  }
  GlobalWire global;
  if (ReadSection(root, "global", &global)) {
    FromWire(global, &parsed.global);
  }
  LlmWire llm;
  if (ReadSection(root, "llm", &llm)) {
    FromWire(llm, &parsed.llm);
  }
  ScenesWire scenes;
  if (ReadSection(root, "scenes", &scenes)) {
    FromWire(scenes, &parsed.scenes);
  }
  AsrWire asr;
  if (ReadSection(root, "asr", &asr)) {
    FromWire(asr, &parsed.asr);
  }

  *config = std::move(parsed);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}
