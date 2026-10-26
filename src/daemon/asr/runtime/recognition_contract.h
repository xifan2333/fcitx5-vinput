#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vinput::daemon::asr {

constexpr std::size_t kMinSamplesForRecognition = 8000;

enum class AudioDeliveryMode : std::uint8_t {
  Buffered,
  Chunked,
};

enum class RecognitionEventKind : std::uint8_t {
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
  RecognitionSession() = default;
  virtual ~RecognitionSession() = default;

  RecognitionSession(const RecognitionSession&) = delete;
  RecognitionSession& operator=(const RecognitionSession&) = delete;
  RecognitionSession(RecognitionSession&&) = delete;
  RecognitionSession& operator=(RecognitionSession&&) = delete;

  virtual bool PushAudio(std::span<const int16_t> pcm, std::string* error) = 0;
  virtual bool PushAudio(std::span<const float> samples, std::string* error);
  virtual bool Finish(std::string* error) = 0;
  virtual void Cancel() = 0;
  virtual void Interrupt() noexcept {}
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
  AsrBackend() = default;
  virtual ~AsrBackend() = default;

  AsrBackend(const AsrBackend&) = delete;
  AsrBackend& operator=(const AsrBackend&) = delete;
  AsrBackend(AsrBackend&&) = delete;
  AsrBackend& operator=(AsrBackend&&) = delete;

  [[nodiscard]] virtual BackendDescriptor Describe() const = 0;
  virtual std::unique_ptr<RecognitionSession> CreateSession(std::string* error) = 0;
};

} // namespace vinput::daemon::asr
