#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <readerwriterqueue.h>
#include <spa/utils/hook.h>
#include <string>
#include <vector>

#include "common/audio/pipewire_device.h"

#include "daemon/audio/audio_types.h"

struct spa_pod;

class AudioCapture {
public:
  // Default preallocated capacity: 2048 chunks (approx 4.3 minutes at 16 kHz).
  static constexpr std::size_t kDefaultQueueCapacity = 2048;

  // Timing for one BeginRecording call (milliseconds; -1 = unknown).
  struct StartTiming {
    long idle_gap_ms = -1;
    long create_stream_ms = -1;
  };

  AudioCapture();
  ~AudioCapture();

  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;
  AudioCapture(AudioCapture&&) = delete;
  AudioCapture& operator=(AudioCapture&&) = delete;

  bool Start(std::string* error = nullptr);
  std::vector<int16_t> StopAndGetBuffer();
  void Stop();
  bool BeginRecording(std::string* error = nullptr, StartTiming* timing = nullptr);
  void EndRecording();
  bool IsRecording() const;
  void SetTargetObject(std::string target_object);

  // SPSC lock-free dequeue for downstream audio consumers.
  // Returns true if a chunk was dequeued, false if queue is empty.
  bool TryDequeue(vinput::audio::AudioPcmChunk& chunk);

  // Set an optional eventfd (non-blocking) signaled when audio chunks are enqueued.
  // Pass -1 to disable.
  void SetNotifyFd(int fd) noexcept;

  // Approximate number of chunks currently waiting in the lock-free queue.
  [[nodiscard]] std::size_t ApproxQueueSize() const;

  // Real-time drop statistics when the queue is full.
  [[nodiscard]] std::size_t DroppedChunks() const noexcept;
  [[nodiscard]] std::size_t DroppedSamples() const noexcept;
  void ResetDropCounters() noexcept;

  // Latency from BeginRecording arm to first non-empty process buffer, if any.
  std::optional<long> FirstBufferLatencyMs() const;

private:
  static void onProcess(void* userdata);
  static void onParamChanged(void* userdata, uint32_t id, const struct spa_pod* param);
  static void onStateChanged(void* userdata, enum pw_stream_state old, enum pw_stream_state state,
                             const char* error);
  bool CreateAndConnectStream(const vinput::pw::ResolvedCaptureTarget& resolved_target,
                              std::string* error);
  void ArmRecording() noexcept;
  void DestroyStream();
  void processCallback();
  void MarkStreamDestroyed();
  std::string CurrentTargetObject() const;

  // SPSC Lock-free ringbuffer and real-time safe drop telemetry
  moodycamel::ReaderWriterQueue<vinput::audio::AudioPcmChunk> queue_{kDefaultQueueCapacity};
  struct pw_thread_loop* loop_ = nullptr;
  std::atomic<struct pw_stream*> stream_{nullptr};
  vinput::pw::ControlPlane control_plane_;
  std::atomic<std::size_t> dropped_chunks_{0};
  std::atomic<std::size_t> dropped_samples_{0};

  // Lock-free first-buffer timing (nanoseconds since epoch; 0 = unrecorded)
  std::atomic<int64_t> recording_armed_ns_{0};
  std::atomic<int64_t> first_buffer_ns_{0};

  // Idle gap timing (main/worker thread, guarded by timing_mutex_)
  std::optional<std::chrono::steady_clock::time_point> last_stream_destroyed_at_;

  std::string target_object_;
  mutable std::mutex target_mutex_;
  mutable std::mutex timing_mutex_;
  std::mutex consumer_mutex_;

  struct pw_stream_events stream_events_{};
  struct spa_hook stream_listener_{};
  std::atomic<int> notify_fd_{-1};
  std::atomic<bool> recording_{false};
};
