#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/utils/hook.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/audio/pipewire_device.h"

#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/audio_ring_buffer.h"
#include "daemon/audio/pipewire_control_plane.h"

struct spa_pod;

class AudioCapture : public vinput::daemon::audio::AudioInputSource {
public:
  // Default preallocated ring buffer capacity: 15 seconds at 16 kHz (< 500 KB footprint).
  static constexpr std::size_t kDefaultCapacitySeconds = 15;

  // Timing for one BeginRecording call (milliseconds; -1 = unknown).
  struct StartTiming {
    long create_stream_ms = -1;
  };

  AudioCapture();
  ~AudioCapture() override;

  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;
  AudioCapture(AudioCapture&&) = delete;
  AudioCapture& operator=(AudioCapture&&) = delete;

  bool Start(std::string* error = nullptr);

  // AudioInputSource interface
  bool Begin(std::string_view target = {}, FirstBufferCallback on_first_buffer = nullptr,
             ErrorCallback on_error = nullptr, std::string* error = nullptr) override;
  std::vector<int16_t> StopAndDrain() override;
  void Abort() override;
  std::size_t DrainAvailable(SampleConsumer consumer) override;
  [[nodiscard]] bool IsRecording() const override;
  [[nodiscard]] std::size_t DroppedSamples() const noexcept override;
  void ResetDropCounters() noexcept override;
  [[nodiscard]] std::optional<long> FirstBufferLatencyMs() const override;

  // Compatibility and timing inspection APIs
  bool BeginRecording(std::string* error = nullptr, StartTiming* timing = nullptr);
  std::vector<int16_t> StopAndGetBuffer();
  void Stop();
  void EndRecording();
  void SetTargetObject(std::string target_object);

  // Set an optional eventfd (non-blocking) signaled when audio samples are enqueued.
  // Pass -1 to disable.
  void SetNotifyFd(int fd) noexcept override;

private:
  static void onProcess(void* userdata);
  static void onParamChanged(void* userdata, uint32_t id, const spa_pod* param);
  static void onStateChanged(void* userdata, pw_stream_state old, pw_stream_state state,
                             const char* error);
  bool CreateAndConnectStream(const vinput::pw::ResolvedCaptureTarget& resolved_target,
                              std::string* error);
  void ArmRecording() noexcept;
  void DestroyStream();
  void DrainQueue();
  void processCallback();
  void MarkStreamDestroyed();
  std::string CurrentTargetObject() const;

  // SPSC compact sample-level ring buffer (< 500 KB)
  vinput::daemon::audio::AudioSampleRingBuffer ring_buffer_{kDefaultCapacitySeconds, 16000};
  pw_thread_loop* loop_ = nullptr;
  std::atomic<pw_stream*> stream_{nullptr};
  vinput::daemon::audio::PipeWireControlPlane control_plane_;
  std::atomic<std::size_t> dropped_samples_{0};

  // Lock-free first-buffer timing (nanoseconds since epoch; 0 = unrecorded)
  std::atomic<int64_t> recording_armed_ns_{0};
  std::atomic<int64_t> first_buffer_ns_{0};
  std::atomic<bool> first_buffer_notified_{false};

  std::string target_object_;
  mutable std::mutex target_mutex_;
  std::mutex consumer_mutex_;
  mutable std::mutex callback_mutex_;
  FirstBufferCallback on_first_buffer_;
  ErrorCallback on_error_;

  pw_stream_events stream_events_{};
  spa_hook stream_listener_{};
  std::atomic<int> notify_fd_{-1};
  std::atomic<bool> recording_{false};
};
