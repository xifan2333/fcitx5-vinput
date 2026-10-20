#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <readerwriterqueue.h>
#include <spa/param/audio/format-utils.h>
#include <span>
#include <string>
#include <vector>

#include "daemon/audio/audio_types.h"

class AudioCapture {
public:
  // Default preallocated capacity: 2048 chunks (approx 4.3 minutes at 16 kHz).
  static constexpr std::size_t kDefaultQueueCapacity = 2048;

  // Timing for one BeginRecording call (milliseconds; -1 = unknown).
  struct StartTiming {
    long idle_gap_ms = -1;
    long create_stream_ms = -1;
    long set_active_ms = -1;
    bool stream_reused = false;
    bool created_new_stream = false;
    bool reuse_policy_enabled = false;
  };

  AudioCapture();
  ~AudioCapture();

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

  // Whether stream-reuse policy is enabled (env VINPUT_CAPTURE_REUSE).
  static bool StreamReuseEnabled();

  // After stop, keep an inactive connected stream for this long (ms), then
  // destroy. Env VINPUT_CAPTURE_IDLE_DESTROY_MS (default 15000). 0 = destroy
  // immediately after stop even when reuse is on.
  static long IdleDestroyMs();

  // Destroy a reusable inactive stream once its idle grace has expired.
  // Safe to call from the daemon main loop. Returns true if a stream was
  // destroyed.
  bool MaybeDestroyExpiredStream();

private:
  static void onProcess(void* userdata);
  static void onParamChanged(void* userdata, uint32_t id, const struct spa_pod* param);
  static void onStateChanged(void* userdata, enum pw_stream_state old, enum pw_stream_state state,
                             const char* error);
  bool CreateStream(bool start_inactive, std::string* error = nullptr);
  bool SetStreamActive(bool active, std::string* error = nullptr);
  void DestroyStream();
  void processCallback();
  void MarkStreamDestroyed();
  void MarkStreamDeactivated();
  bool CanReuseStreamLocked() const;
  std::string CurrentTargetObject() const;

  struct pw_thread_loop* loop_ = nullptr;
  struct pw_stream* stream_ = nullptr;
  struct pw_stream_events stream_events_{};
  std::atomic<bool> recording_{false};
  mutable std::mutex target_mutex_;
  mutable std::mutex timing_mutex_;
  std::mutex consumer_mutex_;
  std::string target_object_;
  std::string connected_target_object_;
  bool stream_active_ = false;

  // SPSC Lock-free ringbuffer and real-time safe drop telemetry
  moodycamel::ReaderWriterQueue<vinput::audio::AudioPcmChunk> queue_{kDefaultQueueCapacity};
  std::atomic<std::size_t> dropped_chunks_{0};
  std::atomic<std::size_t> dropped_samples_{0};
  std::atomic<int> notify_fd_{-1};

  // Lock-free first-buffer timing (nanoseconds since epoch; 0 = unrecorded)
  std::atomic<int64_t> recording_armed_ns_{0};
  std::atomic<int64_t> first_buffer_ns_{0};

  // Stream reuse & idle destroy timing (main-thread only, guarded by timing_mutex_)
  std::optional<std::chrono::steady_clock::time_point> last_stream_destroyed_at_;
  std::optional<std::chrono::steady_clock::time_point> last_stream_deactivated_at_;
  std::optional<std::chrono::steady_clock::time_point> idle_destroy_deadline_;

  void ScheduleIdleDestroy();
  void CancelIdleDestroy();
};
