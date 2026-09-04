#include <algorithm>
#include <iostream>
#include <iterator>
#include <set>
#include <utility>

#include "common/config/core_config.h"

namespace {

constexpr std::string_view kLegacyCommandPrompt =
    "# Command Mode Prompt\n\n"
    "## Role\n\n"
    "You are an assistant that applies a spoken command to the user-provided "
    "text.\n\n"
    "## Context\n\n"
    "- The user message is the source text to operate on.\n"
    "- The spoken command may contain ASR errors.\n"
    "- The spoken command is appended at runtime in the `## Task` section.\n\n"
    "## Task\n";

constexpr std::string_view kShortTagCommandPrompt =
    "# Command Mode Prompt\n\n"
    "## Role\n\n"
    "You are an assistant that applies a spoken command to the selected "
    "text.\n\n"
    "## Input\n\n"
    "The input data is provided in XML tags. Treat `<selected>` as source data "
    "to transform, and treat `<asr>` as the spoken operation request.\n\n"
    "<selected>\n"
    "{{selected}}\n"
    "</selected>\n\n"
    "<asr>\n"
    "{{asr}}\n"
    "</asr>\n\n"
    "## Task\n\n"
    "Interpret the spoken command in `<asr>` and apply it to the source text "
    "in `<selected>`. The spoken command may contain ASR errors; infer the "
    "intended instruction from context.\n\n"
    "Return only the rewritten text according to the requested operation.\n";

constexpr std::string_view kDefaultCommandPrompt =
    "# Command Mode Prompt\n\n"
    "## Role\n\n"
    "You are an assistant that applies a spoken command to the selected "
    "text.\n\n"
    "## Input\n\n"
    "The input data is provided in XML tags. Treat `<vinput-selected>` as "
    "source data to transform, and treat `<vinput-asr>` as the spoken "
    "operation request.\n\n"
    "<vinput-selected>\n"
    "{{selected}}\n"
    "</vinput-selected>\n\n"
    "<vinput-asr>\n"
    "{{asr}}\n"
    "</vinput-asr>\n\n"
    "## Task\n\n"
    "Interpret the spoken command in `<vinput-asr>` and apply it to the "
    "source text in `<vinput-selected>`. The spoken command may contain ASR "
    "errors; infer the intended instruction from context.\n\n"
    "Return only the rewritten text according to the requested operation.\n";

vinput::scene::Definition MakeBuiltinScene(std::string_view id) {
  vinput::scene::Definition scene;
  scene.id = std::string(id);
  scene.builtin = true;
  if (id == vinput::scene::kRawSceneId) {
    scene.label = std::string(vinput::scene::kRawSceneLabelKey);
    scene.llm_max_candidates = 0;
  } else if (id == vinput::scene::kCommandSceneId) {
    scene.label = std::string(vinput::scene::kCommandSceneLabelKey);
    scene.prompt = std::string(kDefaultCommandPrompt);
  }
  vinput::scene::NormalizeDefinition(&scene);
  return scene;
}

template <typename T> void EraseEmptyEnvKeys(T* entry) {
  for (auto it = entry->env.begin(); it != entry->env.end();) {
    it = it->first.empty() ? entry->env.erase(it) : std::next(it);
  }
}

} // namespace

void NormalizeCoreConfig(CoreConfig* config) {
  if (!config) {
    return;
  }

  // Clamp audio-related knobs to sane ranges.
  config->asr.inputGain = std::clamp(config->asr.inputGain, 0.1, 10.0);
  config->global.duckOutputVolume = std::clamp(config->global.duckOutputVolume, 0.0, 1.0);
  config->asr.vad.threshold = std::clamp(config->asr.vad.threshold, 0.05, 0.95);
  config->asr.vad.minSpeechDuration = std::clamp(config->asr.vad.minSpeechDuration, 0.05, 2.0);
  config->asr.vad.minSilenceDuration = std::clamp(config->asr.vad.minSilenceDuration, 0.05, 5.0);
  config->asr.vad.speechPadMs = std::clamp(config->asr.vad.speechPadMs, 0, 2000);

  {
    std::set<std::string> seen;
    std::vector<std::string> normalized;
    for (const auto& url : config->registry.baseUrls) {
      if (!url.empty() && seen.insert(url).second) {
        normalized.push_back(url);
      }
    }
    config->registry.baseUrls = std::move(normalized);
  }

  {
    std::set<std::string> seen;
    std::vector<LlmProvider> normalized;
    for (auto provider : config->llm.providers) {
      if (provider.id.empty()) {
        std::cerr << "Ignoring LLM provider with empty id\n";
        continue;
      }
      if (!seen.insert(provider.id).second) {
        std::cerr << "Ignoring duplicate LLM provider '" << provider.id << "'\n";
        continue;
      }
      normalized.push_back(std::move(provider));
    }
    config->llm.providers = std::move(normalized);
  }

  {
    std::set<std::string> seen;
    std::vector<LlmAdapter> normalized;
    for (auto adapter : config->llm.adapters) {
      if (adapter.id.empty()) {
        std::cerr << "Ignoring LLM adapter with empty id\n";
        continue;
      }
      if (!seen.insert(adapter.id).second) {
        std::cerr << "Ignoring duplicate LLM adapter '" << adapter.id << "'\n";
        continue;
      }
      EraseEmptyEnvKeys(&adapter);
      normalized.push_back(std::move(adapter));
    }
    config->llm.adapters = std::move(normalized);
  }

  {
    std::set<std::string> seen;
    std::vector<AsrProvider> normalized;
    for (auto provider : config->asr.providers) {
      const std::string& id = AsrProviderId(provider);
      if (id.empty()) {
        std::cerr << "Ignoring ASR provider with empty id\n";
        continue;
      }
      if (!seen.insert(id).second) {
        std::cerr << "Ignoring duplicate ASR provider '" << id << "'\n";
        continue;
      }
      if (auto* cmd = std::get_if<CommandAsrProvider>(&provider)) {
        if (cmd->command.empty()) {
          std::cerr << "Ignoring command ASR provider '" << id << "' with empty command\n";
          continue;
        }
        EraseEmptyEnvKeys(cmd);
      }
      normalized.push_back(std::move(provider));
    }
    config->asr.providers = std::move(normalized);
  }

  {
    std::set<std::string> seen;
    std::vector<vinput::scene::Definition> normalized;
    for (auto scene : config->scenes.definitions) {
      vinput::scene::NormalizeDefinition(&scene);
      if (scene.id == vinput::scene::kCommandSceneId &&
          (scene.prompt.empty() || std::string_view(scene.prompt) == kLegacyCommandPrompt ||
           std::string_view(scene.prompt) == kShortTagCommandPrompt)) {
        scene.prompt = std::string(kDefaultCommandPrompt);
      }
      std::string error;
      if (!vinput::scene::ValidateDefinition(scene, &error)) {
        std::cerr << "Ignoring invalid scene '" << scene.id << "': " << error << "\n";
        continue;
      }
      if (!seen.insert(scene.id).second) {
        std::cerr << "Ignoring duplicate scene id '" << scene.id << "'\n";
        continue;
      }
      normalized.push_back(std::move(scene));
    }

    if (!seen.count(std::string(vinput::scene::kRawSceneId))) {
      normalized.push_back(MakeBuiltinScene(vinput::scene::kRawSceneId));
      seen.insert(std::string(vinput::scene::kRawSceneId));
    }
    if (!seen.count(std::string(vinput::scene::kCommandSceneId))) {
      normalized.push_back(MakeBuiltinScene(vinput::scene::kCommandSceneId));
      seen.insert(std::string(vinput::scene::kCommandSceneId));
    }

    config->scenes.definitions = std::move(normalized);
  }

  if (!config->asr.activeProvider.empty() &&
      ResolveAsrProvider(*config, config->asr.activeProvider) == nullptr) {
    config->asr.activeProvider.clear();
  }

  if (!config->scenes.activeScene.empty() &&
      vinput::scene::Find({config->scenes.activeScene, config->scenes.definitions},
                          config->scenes.activeScene) == nullptr) {
    config->scenes.activeScene.clear();
  }
}
