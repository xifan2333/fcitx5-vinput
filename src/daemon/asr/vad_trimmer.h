#pragma once

#include <optional>
#include <sherpa-onnx/c-api/cxx-api.h>
#include <string>
#include <vector>

struct VadTrimParams {
  float threshold = 0.35F;
  float min_speech_duration = 0.10F;
  float min_silence_duration = 0.8F;
  float max_speech_duration = 15.0F;
  int speech_pad_ms = 500;
};

class VadTrimmer {
public:
  VadTrimmer();
  ~VadTrimmer();

  VadTrimmer(const VadTrimmer&) = delete;
  VadTrimmer& operator=(const VadTrimmer&) = delete;

  VadTrimmer(VadTrimmer&&) noexcept;
  VadTrimmer& operator=(VadTrimmer&&) noexcept;

  // Load silero_vad.onnx from `model_path`. Returns true on success.
  bool Init(const std::string& model_path, int sample_rate = 16000,
            const std::string& provider = "cpu", const VadTrimParams& params = {},
            std::string* error = nullptr);

  // Extract continuous speech audio between outer bounds, preserving internal pauses.
  // Returns empty if no speech found.
  std::vector<float> Trim(const std::vector<float>& samples, int sample_rate);

  // Segment audio into speech utterances using VAD, respecting max_speech_duration.
  // Returns empty if no speech is detected.
  [[nodiscard]] std::vector<std::vector<float>> Segment(const std::vector<float>& samples,
                                                        int sample_rate);

  [[nodiscard]] bool Available() const;
  void Shutdown();

private:
  std::optional<sherpa_onnx::cxx::VoiceActivityDetector> vad_;
  int sample_rate_ = 16000;
  VadTrimParams params_{};
};
