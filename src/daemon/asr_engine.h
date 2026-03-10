#pragma once

#include "common/model_manager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SherpaOnnxOfflineRecognizer;

struct AsrConfig {
  std::string language;
  std::vector<std::string> hotwords;
  float hotwords_score = 1.5f;
  int thread_num = 4;
};

class AsrEngine {
public:
  static constexpr std::size_t kMinSamplesForInference = 8000; // 0.5 s @ 16 kHz

  AsrEngine();
  ~AsrEngine();

  bool Init(const ModelInfo &info, const AsrConfig &asr_config);
  std::string Infer(const std::vector<int16_t> &pcm_data);
  void Shutdown();
  bool IsInitialized() const;

private:
  const SherpaOnnxOfflineRecognizer *recognizer_ = nullptr;
  bool initialized_ = false;
  std::string hotwords_tmp_path_;
};
