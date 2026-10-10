#pragma once

#include <string>

struct AsrConfig {
  std::string language;
  std::string hotwords_file;
  int thread_num = 4;
  bool vad_enabled = true;
  std::string vad_model_path;
  float vad_threshold = 0.35F;
  float vad_min_speech_duration = 0.10F;
  float vad_min_silence_duration = 1.2F;
  int vad_speech_pad_ms = 500;
};
