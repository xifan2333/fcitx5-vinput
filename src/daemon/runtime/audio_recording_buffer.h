#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vinput::daemon::runtime {

// Buffering and chunking engine for raw audio samples.
// Access should be serialized by the owner (e.g. DaemonRuntimeController::state_mutex_).
// Absorbs pre-roll startup samples, applies input gain at the device boundary,
// and supports dual delivery modes (batch full-utterance vs. streaming chunking).
class AudioRecordingBuffer {
public:
  static constexpr std::size_t kStreamingChunkSamples = 800;

  AudioRecordingBuffer() = default;

  void Reset(float gain = 1.0F);
  void SetGain(float gain) noexcept;
  [[nodiscard]] float Gain() const noexcept;

  // Pre-roll startup samples arriving while ASR session initializes.
  // Stored un-gained until Activate() is called.
  void AppendStartup(std::span<const int16_t> pcm);

  // Activates the buffer with delivery mode (buffered vs streaming)
  // and migrates any startup samples with gain applied.
  void Activate(bool is_buffered);

  // Ingests live audio samples after activation.
  // Gain is applied at ingestion. If called prior to Activate(),
  // falls back to AppendStartup().
  void Append(std::span<const int16_t> pcm);

  // In streaming mode, pops fixed-size chunks for progressive inference.
  std::optional<std::vector<int16_t>>
  PopStreamingChunk(std::size_t chunk_size = kStreamingChunkSamples);

  // Drains whatever remaining samples exist in the streaming buffer.
  std::vector<int16_t> DrainStreamingTail();

  // In buffered mode, extracts the complete recorded PCM buffer.
  std::vector<int16_t> TakeBufferedPcm();

  // Total samples ingested since the last Reset()/Clear() across all phases.
  // Retains its value even after TakeBufferedPcm() or DrainStreamingTail()
  // so callers can inspect total utterance duration for logging or guards.
  [[nodiscard]] std::size_t TotalSampleCount() const noexcept;

  // Clears all internal buffers, gain, and counters back to default idle state.
  void Clear() noexcept;

private:
  float gain_ = 1.0F;
  bool is_buffered_ = false;
  bool is_activated_ = false;
  std::vector<int16_t> startup_pcm_;
  std::vector<int16_t> buffered_pcm_;
  std::vector<int16_t> streaming_pcm_;
  std::size_t total_samples_ = 0;
};

} // namespace vinput::daemon::runtime
