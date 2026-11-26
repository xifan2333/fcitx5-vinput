#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <pulse/context.h>
#include <pulse/mainloop-api.h>
#include <pulse/stream.h>
#include <string>
#include <string_view>
#include <vector>

#include "common/audio/pulse_device.h"
#include "common/audio/pulse_raii.h"

#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/audio_ring_buffer.h"

class AudioCapture : public vinput::daemon::audio::AudioInputSource {
public:
  // Default preallocated ring buffer capacity: 15 seconds at 16 kHz (< 500 KB footprint).
  static constexpr std::size_t kDefaultCapacitySeconds = 15;

  // Timing for one BeginRecording call (milliseconds; -1 = unknown).
  struct StartTiming {
    long create_stream_ms = -1;
  };

  AudioCapture() = default;
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
  void EndRecording();
  void SetTargetObject(std::string target_object);

  // Set an optional eventfd (non-blocking) signaled when audio samples are enqueued.
  // Pass -1 to disable.
  void SetNotifyFd(int fd) noexcept override;

private:
  static void OnContextState(pa_context* ctx, void* userdata) noexcept;
  static void OnStreamState(pa_stream* stream, void* userdata) noexcept;
  static void OnStreamRead(pa_stream* stream, std::size_t nbytes, void* userdata) noexcept;

  bool EnsureContextConnected(std::string* error, int timeout_ms);
  bool WaitForContextReadyLocked(pa_mainloop_api* api, int timeout_ms, std::string* error);
  void FailContextConnectLocked(int err_code, std::string* error);
  bool CreateAndConnectStream(const vinput::pulse::ResolvedCaptureTarget& resolved_target,
                              std::string* error);
  void DisconnectContext() noexcept;
  void ArmRecording() noexcept;
  void DisconnectStream() noexcept;
  void ClearCallbackState() noexcept;
  void DestroyStream();
  void DrainQueue();
  void DispatchPendingError();
  void ReadStreamFragments(pa_stream* stream) noexcept;
  std::size_t WriteHoleSamples(std::size_t hole_samples) noexcept;
  std::string CurrentTargetObject() const;

  // SPSC compact sample-level ring buffer (< 500 KB)
  vinput::daemon::audio::AudioSampleRingBuffer ring_buffer_{kDefaultCapacitySeconds, 16000};
  vinput::pulse::ScopedThreadedMainloop loop_;
  vinput::pulse::ScopedContext context_;
  vinput::pulse::ScopedStream stream_;
  std::atomic<std::size_t> dropped_samples_{0};

  // Lock-free first-buffer timing (nanoseconds since epoch; 0 = unrecorded)
  std::atomic<int64_t> recording_armed_ns_{0};
  std::atomic<int64_t> first_buffer_ns_{0};
  std::atomic<bool> first_buffer_notified_{false};
  std::atomic<bool> has_pending_error_{false};

  std::string target_object_;
  mutable std::mutex target_mutex_;
  std::mutex consumer_mutex_;
  mutable std::mutex callback_mutex_;
  FirstBufferCallback on_first_buffer_;
  ErrorCallback on_error_;
  std::string pending_error_;

  std::atomic<int> notify_fd_{-1};
  std::atomic<bool> recording_{false};
};
