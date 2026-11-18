#include "audio_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/buffer/buffer.h>
#if __has_include(<spa/param/audio/raw-utils.h>)
#include <spa/param/audio/raw-utils.h>
#else
#include <spa/param/audio/format-utils.h>
#endif
#include <spa/param/audio/raw.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/audio/pipewire_device.h"
#include "common/utils/debug_log.h"
#include "common/utils/scope_guard.h"
#include "common/utils/time_utils.h"

#include "daemon/audio/audio_types.h"

namespace {

pw_properties* BuildCaptureProperties(const vinput::pw::ResolvedCaptureTarget& resolved_target) {
  const char* capture_sink_str = resolved_target.is_sink_capture ? "true" : "false";
  auto* properties =
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                        PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_STREAM_CAPTURE_SINK,
                        capture_sink_str, PW_KEY_NODE_LATENCY, "128/16000", nullptr);
  if (properties == nullptr) {
    return nullptr;
  }

  const std::string& target_value = !resolved_target.object_serial.empty()
                                        ? resolved_target.object_serial
                                        : resolved_target.node_name;
  if (!target_value.empty()) {
    pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target_value.c_str());
    VLOG_INFO("using PipeWire target.object={} (node={}, stream.capture.sink={})", target_value,
              resolved_target.node_name, capture_sink_str);
  }
  return properties;
}

int ConnectStream(pw_stream* stream) {
  uint8_t pod_buffer[1024];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
  spa_audio_info_raw raw_info{};
  raw_info.format = SPA_AUDIO_FORMAT_S16_LE;
  raw_info.rate = 16000;
  raw_info.channels = 1;
  raw_info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw_info);

  const int flags =
      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  return pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                           static_cast<pw_stream_flags>(flags), params, 1);
}

} // namespace

AudioCapture::AudioCapture() {
  pw_init(nullptr, nullptr);
}

AudioCapture::~AudioCapture() {
  DestroyStream();
  control_plane_.Disconnect();
  if (loop_ != nullptr) {
    pw_thread_loop_stop(loop_);
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
  pw_deinit();
}

void AudioCapture::onProcess(void* userdata) {
  auto* self = static_cast<AudioCapture*>(userdata);
  self->processCallback();
}

void AudioCapture::processCallback() {
  pw_stream* s = stream_.load(std::memory_order_acquire);
  if (s == nullptr) {
    return;
  }

  pw_buffer* b = pw_stream_dequeue_buffer(s);
  if (b == nullptr) {
    return;
  }

  const auto cleanup_buffer = vinput::MakeScopeExit([s, b] { pw_stream_queue_buffer(s, b); });

  const spa_buffer* buf = b->buffer;
  if (buf == nullptr || buf->n_datas == 0 || buf->datas[0].data == nullptr ||
      buf->datas[0].chunk == nullptr) {
    return;
  }

  if (!recording_.load(std::memory_order_relaxed)) {
    return;
  }

  const auto* raw = static_cast<const uint8_t*>(buf->datas[0].data);
  const uint32_t offset = buf->datas[0].chunk->offset;
  const uint32_t size = buf->datas[0].chunk->size;
  if (size == 0 || offset + size > buf->datas[0].maxsize) {
    return;
  }

  const uint32_t n_samples = size / sizeof(int16_t);
  if (n_samples == 0) {
    return;
  }

  int64_t expected_zero = 0;
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  first_buffer_ns_.compare_exchange_strong(expected_zero, now_ns, std::memory_order_relaxed);

  bool enqueued_any = false;
  uint32_t offset_samples = 0;
  while (offset_samples < n_samples) {
    const uint32_t to_copy =
        std::min<uint32_t>(n_samples - offset_samples, vinput::audio::kMaxChunkSamples);
    vinput::audio::AudioPcmChunk chunk;
    std::memcpy(chunk.samples.data(), raw + offset + (offset_samples * sizeof(int16_t)),
                to_copy * sizeof(int16_t));
    chunk.size = to_copy;

    if (!queue_.try_enqueue(chunk)) {
      dropped_chunks_.fetch_add(1, std::memory_order_relaxed);
      dropped_samples_.fetch_add(chunk.size, std::memory_order_relaxed);
    } else {
      enqueued_any = true;
    }
    offset_samples += to_copy;
  }

  if (enqueued_any) {
    const int notify_fd = notify_fd_.load(std::memory_order_relaxed);
    if (notify_fd >= 0) {
      const uint64_t val = 1;
      (void)write(notify_fd, &val, sizeof(val));
    }
  }
}

void AudioCapture::onParamChanged(void* userdata, uint32_t id, const spa_pod* param) {
  (void)userdata;
  if (param == nullptr || id != SPA_PARAM_Format) {
    return;
  }

  spa_audio_info_raw info{};
  if (spa_format_audio_raw_parse(param, &info) < 0) {
    return;
  }

  VLOG_INFO("negotiated format: rate={} channels={} fmt={}", info.rate, info.channels,
            static_cast<int>(info.format));
}

void AudioCapture::onStateChanged(void* userdata, pw_stream_state old, pw_stream_state state,
                                  const char* error) {
  (void)userdata;
  const char* err_str = (error != nullptr && error[0] != '\0') ? error : nullptr;
  VLOG_DEBUG("capture stream state: {} -> {}{}{}", pw_stream_state_as_string(old),
             pw_stream_state_as_string(state), err_str != nullptr ? " error=" : "",
             err_str != nullptr ? err_str : "");
  if (state == PW_STREAM_STATE_ERROR) {
    VLOG_ERROR("PipeWire stream error: {}", err_str != nullptr ? err_str : "unknown error");
  }
}

bool AudioCapture::Start(std::string* error) {
  if (loop_ != nullptr) {
    return true;
  }

  loop_ = pw_thread_loop_new("vinput-capture-loop", nullptr);
  if (loop_ == nullptr) {
    if (error != nullptr) {
      *error = "failed to create PipeWire thread loop";
    }
    return false;
  }

  const int ret = pw_thread_loop_start(loop_);
  if (ret < 0) {
    if (error != nullptr) {
      *error = std::string("failed to start PipeWire thread loop: ") + strerror(-ret);
    }
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    return false;
  }

  // Keep only the PipeWire control-plane registry/metadata connection warm at startup;
  // never create or link an audio capture stream until BeginRecording().
  if (!control_plane_.Connect(loop_)) {
    VLOG_DEBUG("PipeWire control plane connection deferred; will retry on capture");
  }

  return true;
}

std::string AudioCapture::CurrentTargetObject() const {
  const std::scoped_lock lock(target_mutex_);
  return target_object_;
}

bool AudioCapture::CreateAndConnectStream(const vinput::pw::ResolvedCaptureTarget& resolved_target,
                                          std::string* error) {
  if (loop_ == nullptr) {
    if (error != nullptr) {
      *error = "audio capture loop is not initialized";
    }
    return false;
  }

  stream_events_.version = PW_VERSION_STREAM_EVENTS;
  stream_events_.param_changed = onParamChanged;
  stream_events_.process = onProcess;
  stream_events_.state_changed = onStateChanged;

  pw_thread_loop_lock(loop_);
  const auto loop_unlock = vinput::MakeScopeExit([this] { pw_thread_loop_unlock(loop_); });

  pw_core* core = control_plane_.Core();
  if (core == nullptr) {
    if (error != nullptr) {
      *error = "PipeWire core connection is not ready";
    }
    return false;
  }

  auto* properties = BuildCaptureProperties(resolved_target);
  if (properties == nullptr) {
    if (error != nullptr) {
      *error = "failed to allocate PipeWire properties";
    }
    return false;
  }

  pw_stream* new_stream = pw_stream_new(core, "vinput-capture", properties);
  if (new_stream == nullptr) {
    if (error != nullptr) {
      *error = "failed to create PipeWire stream";
    }
    return false;
  }

  spa_zero(stream_listener_);
  pw_stream_add_listener(new_stream, &stream_listener_, &stream_events_, this);
  stream_.store(new_stream, std::memory_order_release);

  const int ret = ConnectStream(new_stream);
  if (ret < 0) {
    if (error != nullptr) {
      *error = std::string("failed to connect PipeWire stream: ") + strerror(-ret);
    }
    spa_hook_remove(&stream_listener_);
    pw_stream_destroy(new_stream);
    stream_.store(nullptr, std::memory_order_release);
    return false;
  }

  return true;
}

void AudioCapture::MarkStreamDestroyed() {
  const std::scoped_lock lock(timing_mutex_);
  last_stream_destroyed_at_ = std::chrono::steady_clock::now();
  recording_armed_ns_.store(0, std::memory_order_release);
  first_buffer_ns_.store(0, std::memory_order_release);
}

void AudioCapture::DestroyStream() {
  recording_.store(false, std::memory_order_relaxed);
  if (loop_ == nullptr || stream_.load(std::memory_order_acquire) == nullptr) {
    return;
  }

  pw_thread_loop_lock(loop_);
  if (pw_stream* s = stream_.load(std::memory_order_acquire); s != nullptr) {
    spa_hook_remove(&stream_listener_);
    pw_stream_destroy(s);
    stream_.store(nullptr, std::memory_order_release);
  }
  pw_thread_loop_unlock(loop_);
  MarkStreamDestroyed();
}

void AudioCapture::DrainQueue() {
  const std::scoped_lock lock(consumer_mutex_);
  vinput::audio::AudioPcmChunk discard;
  while (queue_.try_dequeue(discard)) {
  }
}

void AudioCapture::ArmRecording() noexcept {
  const int64_t begin_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
  first_buffer_ns_.store(0, std::memory_order_release);
  recording_armed_ns_.store(begin_ns, std::memory_order_release);
}

bool AudioCapture::BeginRecording(std::string* error, StartTiming* timing) {
  if (loop_ == nullptr) {
    if (error != nullptr) {
      *error = "audio capture loop is not initialized";
    }
    return false;
  }

  StartTiming local_timing;
  const auto begin_at = std::chrono::steady_clock::now();
  {
    const std::scoped_lock lock(timing_mutex_);
    if (last_stream_destroyed_at_.has_value()) {
      local_timing.idle_gap_ms =
          vinput::time::MillisecondsBetween(*last_stream_destroyed_at_, begin_at);
    }
  }

  DrainQueue();
  ResetDropCounters();

  DestroyStream();
  if (!control_plane_.EnsureConnected(loop_)) {
    if (error != nullptr) {
      *error = "PipeWire control plane is unavailable";
    }
    return false;
  }

  const auto resolved_target = control_plane_.ResolveTarget(CurrentTargetObject());

  ArmRecording();
  recording_.store(true, std::memory_order_relaxed);

  const auto create_start = std::chrono::steady_clock::now();
  if (!CreateAndConnectStream(resolved_target, error)) {
    recording_.store(false, std::memory_order_relaxed);
    recording_armed_ns_.store(0, std::memory_order_release);
    return false;
  }
  local_timing.create_stream_ms =
      vinput::time::MillisecondsBetween(create_start, std::chrono::steady_clock::now());

  VLOG_DEBUG("capture begin idle_gap_ms={} create_stream_ms={}", local_timing.idle_gap_ms,
             local_timing.create_stream_ms);

  if (timing != nullptr) {
    *timing = local_timing;
  }
  return true;
}

void AudioCapture::EndRecording() {
  DestroyStream();
  DrainQueue();
}

std::vector<int16_t> AudioCapture::StopAndGetBuffer() {
  DestroyStream();

  const std::scoped_lock lock(consumer_mutex_);
  std::vector<int16_t> result;
  vinput::audio::AudioPcmChunk chunk;
  while (queue_.try_dequeue(chunk)) {
    result.insert(result.end(), chunk.samples.begin(), chunk.samples.begin() + chunk.size);
  }
  return result;
}

void AudioCapture::Stop() {
  EndRecording();
}

bool AudioCapture::IsRecording() const {
  return recording_.load(std::memory_order_relaxed);
}

bool AudioCapture::TryDequeue(vinput::audio::AudioPcmChunk& chunk) {
  const std::scoped_lock lock(consumer_mutex_);
  return queue_.try_dequeue(chunk);
}

void AudioCapture::SetNotifyFd(int fd) noexcept {
  notify_fd_.store(fd, std::memory_order_release);
}

std::size_t AudioCapture::ApproxQueueSize() const {
  return queue_.size_approx();
}

std::size_t AudioCapture::DroppedChunks() const noexcept {
  return dropped_chunks_.load(std::memory_order_relaxed);
}

std::size_t AudioCapture::DroppedSamples() const noexcept {
  return dropped_samples_.load(std::memory_order_relaxed);
}

void AudioCapture::ResetDropCounters() noexcept {
  dropped_chunks_.store(0, std::memory_order_relaxed);
  dropped_samples_.store(0, std::memory_order_relaxed);
}

std::optional<long> AudioCapture::FirstBufferLatencyMs() const {
  const int64_t armed = recording_armed_ns_.load(std::memory_order_acquire);
  const int64_t first = first_buffer_ns_.load(std::memory_order_acquire);
  if (armed <= 0 || first <= 0 || first < armed) {
    return std::nullopt;
  }
  return static_cast<long>((first - armed) / 1'000'000L);
}

void AudioCapture::SetTargetObject(std::string target_object) {
  const std::scoped_lock lock(target_mutex_);
  target_object_ = std::move(target_object);
}
