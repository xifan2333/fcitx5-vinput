#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vinput::daemon::asr {

constexpr std::size_t kMinSamplesForRecognition = 8000;

enum class AudioDeliveryMode {
  Buffered,
  Chunked,
};

enum class RecognitionEventKind {
  PartialText,
  FinalText,
  Error,
  Completed,
};

struct RecognitionEvent {
  RecognitionEventKind kind = RecognitionEventKind::Completed;
  std::string text;
  std::string error;
};

struct BackendCapabilities {
  AudioDeliveryMode audio_delivery_mode = AudioDeliveryMode::Chunked;
  bool supports_streaming = false;
  bool supports_partial_results = false;
  bool requires_network = false;
  bool supports_hotwords = false;
};

struct BackendDescriptor {
  std::string provider_id;
  std::string provider_type;
  std::string backend_id;
  BackendCapabilities capabilities;
};

class RecognitionSession {
public:
  virtual ~RecognitionSession() = default;

  virtual bool PushAudio(std::span<const int16_t> pcm, std::string* error) = 0;
  virtual bool PushAudio(std::span<const float> samples, std::string* error);
  virtual bool Finish(std::string* error) = 0;
  virtual void Cancel() = 0;
  virtual std::vector<RecognitionEvent> PollEvents() = 0;
};

inline bool RecognitionSession::PushAudio(std::span<const float> samples, std::string* error) {
  if (samples.empty()) {
    return PushAudio(std::span<const int16_t>{}, error);
  }
  std::vector<int16_t> pcm(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const float val = samples[i];
    if (!std::isfinite(val)) {
      pcm[i] = 0;
    } else {
      pcm[i] = static_cast<int16_t>(std::clamp(val * 32768.0F, -32768.0F, 32767.0F));
    }
  }
  return PushAudio(pcm, error);
}

class AsrBackend {
public:
  virtual ~AsrBackend() = default;

  virtual BackendDescriptor Describe() const = 0;
  virtual std::unique_ptr<RecognitionSession> CreateSession(std::string* error) = 0;
};

} // namespace vinput::daemon::asr
