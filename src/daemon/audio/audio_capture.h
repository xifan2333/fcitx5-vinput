#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format-utils.h>
#include <span>
#include <string>
#include <vector>

class AudioCapture {
public:
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

  using ChunkCallback = std::function<void(std::span<const int16_t>)>;

  bool Start(std::string* error = nullptr);
  std::vector<int16_t> StopAndGetBuffer();
  void Stop();
  bool BeginRecording(std::string* error = nullptr, StartTiming* timing = nullptr);
  void EndRecording();
  bool IsRecording() const;
  void SetTargetObject(std::string target_object);
  void SetChunkCallback(ChunkCallback callback);

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
  std::mutex buffer_mutex_;
  std::mutex callback_mutex_;
  mutable std::mutex target_mutex_;
  mutable std::mutex timing_mutex_;
  std::vector<int16_t> pcm_buffer_;
  std::string target_object_;
  std::string connected_target_object_;
  ChunkCallback chunk_callback_;
  bool stream_active_ = false;
  std::optional<std::chrono::steady_clock::time_point> last_stream_destroyed_at_;
  std::optional<std::chrono::steady_clock::time_point> last_stream_deactivated_at_;
  std::optional<std::chrono::steady_clock::time_point> idle_destroy_deadline_;
  std::optional<std::chrono::steady_clock::time_point> recording_armed_at_;
  std::optional<std::chrono::steady_clock::time_point> first_buffer_at_;

  void ScheduleIdleDestroy();
  void CancelIdleDestroy();
};
