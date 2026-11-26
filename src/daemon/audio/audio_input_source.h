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

  AudioInputSource(const AudioInputSource&) = delete;
  AudioInputSource& operator=(const AudioInputSource&) = delete;
  AudioInputSource(AudioInputSource&&) = delete;
  AudioInputSource& operator=(AudioInputSource&&) = delete;

  // Starts capturing audio from `target` (empty or "default" uses system default).
  // `on_first_buffer` is invoked once (on DrainAvailable) with the steady timestamp (ns)
  // when the first non-empty PCM buffer arrived.
  // `on_error` is invoked if the underlying audio stream encounters an error.
  virtual bool Begin(std::string_view target, FirstBufferCallback on_first_buffer,
                     ErrorCallback on_error, std::string* error) = 0;

  // Stops active capture and drains any remaining buffered PCM samples.
  virtual std::vector<int16_t> StopAndDrain() = 0;

  // Immediately aborts active capture and discards buffered PCM samples.
  virtual void Abort() = 0;

  // Drains currently available PCM chunks into `consumer`. Returns sample count drained.
  virtual std::size_t DrainAvailable(SampleConsumer consumer) = 0;

  // Sets an optional non-blocking eventfd signaled when new samples or stream errors arrive.
  virtual void SetNotifyFd(int fd) noexcept = 0;

  [[nodiscard]] virtual bool IsRecording() const = 0;
  [[nodiscard]] virtual std::size_t DroppedSamples() const noexcept = 0;
  virtual void ResetDropCounters() noexcept = 0;
  [[nodiscard]] virtual std::optional<long> FirstBufferLatencyMs() const = 0;
};

} // namespace vinput::daemon::audio
