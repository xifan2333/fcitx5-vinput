#pragma once

#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format-utils.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class AudioCapture {
public:
  AudioCapture();
  ~AudioCapture();

  bool Start();
  std::vector<int16_t> StopAndGetBuffer();
  bool BeginRecording();
  void EndRecording();
  bool IsRecording() const;
  void SetTargetObject(std::string target_object);

private:
  static void onProcess(void *userdata);
  static void onParamChanged(void *userdata, uint32_t id,
                             const struct spa_pod *param);
  bool CreateStream();
  void DestroyStream();
  void processCallback();

  struct pw_thread_loop *loop_ = nullptr;
  struct pw_stream *stream_ = nullptr;
  struct pw_stream_events stream_events_{};
  std::atomic<bool> recording_{false};
  std::mutex buffer_mutex_;
  std::mutex target_mutex_;
  std::vector<int16_t> pcm_buffer_;
  std::string target_object_;
};
