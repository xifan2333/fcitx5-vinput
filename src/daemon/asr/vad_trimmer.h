#pragma once

#include <string>
#include <vector>

struct SherpaOnnxVoiceActivityDetector;

struct VadTrimParams {
  float threshold = 0.35F;
  float min_speech_duration = 0.10F;
  float min_silence_duration = 1.2F;
  int speech_pad_ms = 500;
};

class VadTrimmer {
public:
  VadTrimmer();
  ~VadTrimmer();

  VadTrimmer(const VadTrimmer&) = delete;
  VadTrimmer& operator=(const VadTrimmer&) = delete;

  // Load silero_vad.onnx from `model_path`. Returns true on success.
  bool Init(const std::string& model_path, int sample_rate = 16000,
            const std::string& provider = "cpu", const VadTrimParams& params = {},
            std::string* error = nullptr);

  // Extract continuous speech audio between outer bounds, preserving internal pauses.
  // Returns empty if no speech found.
  std::vector<float> Trim(const std::vector<float>& samples, int sample_rate);

  bool Available() const;
  void Shutdown();

private:
  const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
  int sample_rate_ = 16000;
  VadTrimParams params_{};
};
