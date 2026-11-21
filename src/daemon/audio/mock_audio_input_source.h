#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/utils/time_utils.h"
#include "common/utils/unique_fd.h"

#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/audio_ring_buffer.h"

namespace vinput::daemon::audio {

class MockAudioInputSource : public AudioInputSource {
public:
  explicit MockAudioInputSource(std::size_t capacity_seconds = 15, std::size_t sample_rate = 16000)
      : ring_buffer_(capacity_seconds, sample_rate) {}

  void SetFailOnBegin(bool fail, std::string error_message = "simulated begin failure") {
    const std::scoped_lock lock(mutex_);
    fail_on_begin_ = fail;
    fail_begin_message_ = std::move(error_message);
  }

  bool Begin(std::string_view target = {}, FirstBufferCallback on_first_buffer = nullptr,
             ErrorCallback on_error = nullptr, std::string* error = nullptr) override {
    const std::scoped_lock lock(mutex_);
    if (fail_on_begin_) {
      if (error != nullptr) {
        *error = fail_begin_message_;
      }
      return false;
    }
    target_ = std::string(target);
    on_first_buffer_ = std::move(on_first_buffer);
    on_error_ = std::move(on_error);
    recording_ = true;
    recording_armed_ns_ = vinput::time::SteadyNowNs();
    first_buffer_ns_ = 0;
    ring_buffer_.Clear();
    return true;
  }

  std::vector<int16_t> StopAndDrain() override {
    const std::scoped_lock lock(mutex_);
    recording_ = false;
    return ring_buffer_.DrainAll();
  }

  void Abort() override {
    const std::scoped_lock lock(mutex_);
    recording_ = false;
    ring_buffer_.Clear();
  }

  std::size_t DrainAvailable(SampleConsumer consumer) override {
    return ring_buffer_.DrainAvailable(std::move(consumer));
  }

  [[nodiscard]] bool IsRecording() const override {
    const std::scoped_lock lock(mutex_);
    return recording_;
  }

  [[nodiscard]] std::size_t DroppedSamples() const noexcept override {
    return dropped_samples_.load(std::memory_order_relaxed);
  }

  void ResetDropCounters() noexcept override {
    dropped_samples_.store(0, std::memory_order_relaxed);
  }

  void SetNotifyFd(int fd) noexcept override { notify_fd_.store(fd, std::memory_order_release); }

  void SetSimulatedLatencyMs(std::optional<long> latency_ms) {
    const std::scoped_lock lock(mutex_);
    simulated_latency_ms_ = latency_ms;
  }

  [[nodiscard]] std::optional<long> FirstBufferLatencyMs() const override {
    const std::scoped_lock lock(mutex_);
    if (simulated_latency_ms_.has_value()) {
      return simulated_latency_ms_;
    }
    return vinput::time::ElapsedMsFromNs(recording_armed_ns_, first_buffer_ns_);
  }

  // --- Test Injection APIs ---

  // Injects simulated PCM audio samples into the ring buffer.
  std::size_t PushSamples(std::span<const int16_t> samples) {
    FirstBufferCallback first_cb;
    int64_t now_ns = 0;
    {
      const std::scoped_lock lock(mutex_);
      if (!recording_) {
        return 0;
      }
      const std::size_t written = ring_buffer_.Write(samples);
      if (written < samples.size()) {
        dropped_samples_.fetch_add(samples.size() - written, std::memory_order_relaxed);
      }
      if (written > 0 && first_buffer_ns_ == 0) {
        now_ns = vinput::time::SteadyNowNs();
        first_buffer_ns_ = now_ns;
        first_cb = on_first_buffer_;
      }
    }
    if (first_cb) {
      first_cb(now_ns);
    }
    vinput::file::SignalEventFd(notify_fd_.load(std::memory_order_acquire));
    return samples.size();
  }

  // Simulates an asynchronous runtime error event (e.g. device disconnect / stream error).
  void SimulateError(std::string_view error_message) {
    ErrorCallback err_cb;
    {
      const std::scoped_lock lock(mutex_);
      recording_ = false;
      err_cb = on_error_;
    }
    if (err_cb) {
      err_cb(error_message);
    }
    vinput::file::SignalEventFd(notify_fd_.load(std::memory_order_acquire));
  }

  [[nodiscard]] std::string LastTarget() const {
    const std::scoped_lock lock(mutex_);
    return target_;
  }

private:
  mutable std::mutex mutex_;
  AudioSampleRingBuffer ring_buffer_;
  bool recording_{false};
  bool fail_on_begin_{false};
  std::string fail_begin_message_;
  std::string target_;
  FirstBufferCallback on_first_buffer_;
  ErrorCallback on_error_;
  std::atomic<int> notify_fd_{-1};
  std::atomic<std::size_t> dropped_samples_{0};
  int64_t recording_armed_ns_{0};
  int64_t first_buffer_ns_{0};
  std::optional<long> simulated_latency_ms_;
};

} // namespace vinput::daemon::audio
