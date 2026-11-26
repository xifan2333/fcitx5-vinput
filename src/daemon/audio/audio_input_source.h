#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vinput::daemon::audio {

// Narrow interface contract for audio input sources (PulseAudio capture or testing mock).
class AudioInputSource {
public:
  using FirstBufferCallback = std::function<void(int64_t timestamp_ns)>;
  using ErrorCallback = std::function<void(std::string_view error)>;
  using SampleConsumer = std::function<void(std::span<const int16_t> pcm)>;

  AudioInputSource() = default;
  virtual ~AudioInputSource() = default;
  AudioInputSource(const AudioInputSource&) = default;
  AudioInputSource& operator=(const AudioInputSource&) = default;
  AudioInputSource(AudioInputSource&&) = default;
  AudioInputSource& operator=(AudioInputSource&&) = default;

  // Starts capturing audio from the resolved target source.
  // When the first audio buffer is received, on_first_buffer (if provided) is invoked.
  // If an asynchronous stream error occurs, on_error (if provided) is invoked.
  virtual bool Begin(std::string_view target = {}, FirstBufferCallback on_first_buffer = nullptr,
                     ErrorCallback on_error = nullptr, std::string* error = nullptr) = 0;

  // Stops capture and drains all remaining buffered audio samples.
  virtual std::vector<int16_t> StopAndDrain() = 0;

  // Aborts capture immediately, discarding unconsumed audio.
  virtual void Abort() = 0;

  // Drains currently available audio samples to the consumer callback.
  virtual std::size_t DrainAvailable(SampleConsumer consumer) = 0;

  // Set an optional eventfd (non-blocking) signaled when audio samples are enqueued.
  // Pass -1 to disable.
  virtual void SetNotifyFd(int /*fd*/) noexcept {}

  // True if currently recording.
  [[nodiscard]] virtual bool IsRecording() const = 0;

  // Real-time drop statistics when queue overflows.
  [[nodiscard]] virtual std::size_t DroppedSamples() const noexcept = 0;
  virtual void ResetDropCounters() noexcept = 0;

  // First buffer latency in milliseconds, if recorded.
  [[nodiscard]] virtual std::optional<long> FirstBufferLatencyMs() const = 0;
};

} // namespace vinput::daemon::audio
