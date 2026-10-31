#include "daemon/runtime/audio_recording_buffer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "daemon/audio/audio_utils.h"

namespace vinput::daemon::runtime {

void AudioRecordingBuffer::Reset(float gain) {
  gain_ = gain;
  is_buffered_ = false;
  is_activated_ = false;
  startup_pcm_.clear();
  buffered_pcm_.clear();
  streaming_pcm_.clear();
  total_samples_ = 0;
}

void AudioRecordingBuffer::SetGain(float gain) noexcept {
  gain_ = gain;
}

float AudioRecordingBuffer::Gain() const noexcept {
  return gain_;
}

void AudioRecordingBuffer::AppendStartup(std::span<const int16_t> pcm) {
  if (pcm.empty()) {
    return;
  }
  if (is_activated_) {
    Append(pcm);
    return;
  }
  startup_pcm_.insert(startup_pcm_.end(), pcm.begin(), pcm.end());
  total_samples_ += pcm.size();
}

void AudioRecordingBuffer::Activate(bool is_buffered) {
  is_activated_ = true;
  is_buffered_ = is_buffered;

  if (!startup_pcm_.empty()) {
    vinput::audio::ApplyGainI16(startup_pcm_, gain_);
    if (is_buffered_) {
      buffered_pcm_ = std::move(startup_pcm_);
    } else {
      streaming_pcm_ = std::move(startup_pcm_);
    }
    startup_pcm_.clear();
  }
}

void AudioRecordingBuffer::Append(std::span<const int16_t> pcm) {
  if (pcm.empty()) {
    return;
  }
  if (!is_activated_) {
    AppendStartup(pcm);
    return;
  }

  total_samples_ += pcm.size();

  std::vector<int16_t> scratch;
  const auto pcm_view = vinput::audio::PrepareGainAdjustedInput(pcm, gain_, &scratch);

  if (is_buffered_) {
    buffered_pcm_.insert(buffered_pcm_.end(), pcm_view.begin(), pcm_view.end());
  } else {
    streaming_pcm_.insert(streaming_pcm_.end(), pcm_view.begin(), pcm_view.end());
  }
}

std::optional<std::vector<int16_t>>
AudioRecordingBuffer::PopStreamingChunk(std::size_t chunk_size) {
  if (chunk_size == 0 || streaming_pcm_.size() < chunk_size) {
    return std::nullopt;
  }
  const auto chunk_end = streaming_pcm_.begin() + static_cast<std::ptrdiff_t>(chunk_size);
  std::vector<int16_t> chunk(streaming_pcm_.begin(), chunk_end);
  streaming_pcm_.erase(streaming_pcm_.begin(), chunk_end);
  return chunk;
}

std::vector<int16_t> AudioRecordingBuffer::DrainStreamingTail() {
  std::vector<int16_t> tail = std::move(streaming_pcm_);
  streaming_pcm_.clear();
  return tail;
}

std::vector<int16_t> AudioRecordingBuffer::TakeBufferedPcm() {
  std::vector<int16_t> result = std::move(buffered_pcm_);
  buffered_pcm_.clear();
  return result;
}

std::size_t AudioRecordingBuffer::TotalSampleCount() const noexcept {
  return total_samples_;
}

void AudioRecordingBuffer::Clear() noexcept {
  gain_ = 1.0F;
  is_buffered_ = false;
  is_activated_ = false;
  startup_pcm_.clear();
  buffered_pcm_.clear();
  streaming_pcm_.clear();
  total_samples_ = 0;
}

} // namespace vinput::daemon::runtime
