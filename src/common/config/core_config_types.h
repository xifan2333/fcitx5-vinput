#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

#include "common/scene/postprocess_scene.h"

namespace vinput::asr {

inline constexpr char kLocalProviderType[] = "local";
inline constexpr char kCommandProviderType[] = "command";

} // namespace vinput::asr

struct LlmProvider {
  std::string id;
  std::string base_url;
  std::string api_key;
  nlohmann::json extra_body = nlohmann::json::object();
};

struct LlmAdapter {
  std::string id;
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  bool autoStart{false};
};

struct AsrProviderBase {
  std::string id;
  int timeoutMs = 0;
};

struct LocalAsrProvider : AsrProviderBase {
  std::string model;
  std::string hotwordsFile;
};

struct CommandAsrProvider : AsrProviderBase {
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
};

using AsrProvider = std::variant<LocalAsrProvider, CommandAsrProvider>;

// Helper to get the id from any AsrProvider variant
inline const std::string& AsrProviderId(const AsrProvider& p) {
  return std::visit([](const AsrProviderBase& base) -> const std::string& { return base.id; }, p);
}

inline int AsrProviderTimeoutMs(const AsrProvider& p) {
  return std::visit([](const AsrProviderBase& base) { return base.timeoutMs; }, p);
}

inline std::string_view AsrProviderType(const AsrProvider& p) {
  if (std::holds_alternative<LocalAsrProvider>(p))
    return vinput::asr::kLocalProviderType;
  return vinput::asr::kCommandProviderType;
}

struct CoreConfig {
  int version = 0;
  struct Registry {
    std::vector<std::string> baseUrls;
  } registry;
  struct Global {
    std::string captureDevice;
    bool duckOutputWhileRecording{false};
    double duckOutputVolume{0.25};
  } global;

  struct Llm {
    std::vector<LlmProvider> providers;
    std::vector<LlmAdapter> adapters;
  } llm;

  struct Asr {
    std::string activeProvider;
    bool normalizeAudio{true};
    double inputGain{1.0};
    struct Vad {
      bool enabled{true};
      // Silero knobs for offline trim (ignored when disabled / non-offline).
      // Tuned for high-recall voice onset detection and natural pause tolerance:
      // - threshold 0.35 captures unvoiced consonants (f/s/p/t/k) with low acoustic energy.
      // - speechPadMs 500 covers Voice Onset Time (VOT) and consonant burst envelopes.
      // - minSilenceDuration 1.2s avoids splitting across typical clause/comma pauses (350-700ms).
      // - minSpeechDuration 0.10s avoids dropping short monosyllables.
      double threshold{0.35};
      double minSpeechDuration{0.10};
      double minSilenceDuration{1.2};
      int speechPadMs{500};
    } vad;
    std::vector<AsrProvider> providers;
  } asr;

  struct Scenes {
    std::string activeScene;
    std::vector<vinput::scene::Definition> definitions;
  } scenes;
};
