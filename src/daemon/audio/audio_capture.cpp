#include "audio_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pipewire/stream.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>

#include "common/utils/debug_log.h"

namespace {

long MillisecondsBetween(std::chrono::steady_clock::time_point start,
                         std::chrono::steady_clock::time_point end) {
  return static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

bool EnvTruthy(const char* value) {
  if (!value || value[0] == '\0') {
    return false;
  }
  return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' ||
         value[0] == 'Y';
}

bool EnvFalsey(const char* value) {
  if (!value || value[0] == '\0') {
    return false;
  }
  return value[0] == '0' || value[0] == 'f' || value[0] == 'F' || value[0] == 'n' ||
         value[0] == 'N';
}

} // namespace

bool AudioCapture::StreamReuseEnabled() {
  // Default ON. Set VINPUT_CAPTURE_REUSE=0 to force legacy destroy/create.
  const char* value = std::getenv("VINPUT_CAPTURE_REUSE");
  if (!value) {
    return true;
  }
  if (EnvFalsey(value)) {
    return false;
  }
  if (EnvTruthy(value)) {
    return true;
  }
  return true;
}

long AudioCapture::IdleDestroyMs() {
  // Keep an inactive connected stream briefly after stop so a cold re-press
  // can set_active instead of full reconnect. Default 15s. 0 = destroy ASAP.
  const char* value = std::getenv("VINPUT_CAPTURE_IDLE_DESTROY_MS");
  if (!value || value[0] == '\0') {
    return 15000;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || parsed < 0) {
    return 15000;
  }
  // Cap to 10 minutes to avoid accidental always-on connect.
  return std::min(parsed, 600000L);
}

AudioCapture::AudioCapture() {
  pw_init(nullptr, nullptr);
}

AudioCapture::~AudioCapture() {
  DestroyStream();
  if (loop_) {
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
  struct pw_stream* s = stream_;
  if (!s) {
    return;
  }

  struct pw_buffer* b = pw_stream_dequeue_buffer(s);
  if (!b) {
    return;
  }

  struct spa_buffer* buf = b->buffer;
  if (!buf || buf->n_datas == 0 || buf->datas[0].data == nullptr ||
      buf->datas[0].chunk == nullptr) {
    pw_stream_queue_buffer(s, b);
    return;
  }

  if (recording_.load(std::memory_order_relaxed)) {
    auto* raw = static_cast<uint8_t*>(buf->datas[0].data);
    const uint32_t offset = buf->datas[0].chunk->offset;
    const uint32_t size = buf->datas[0].chunk->size;
    if (size > 0 && offset + size <= buf->datas[0].maxsize) {
      auto* samples = reinterpret_cast<int16_t*>(raw + offset);
      const uint32_t n_samples = size / sizeof(int16_t);
      {
        const std::scoped_lock lock(timing_mutex_);
        if (recording_armed_at_.has_value() && !first_buffer_at_.has_value()) {
          first_buffer_at_ = std::chrono::steady_clock::now();
          vinput::debug::Log("capture first buffer after %ld ms (samples=%u)\n",
                             MillisecondsBetween(*recording_armed_at_, *first_buffer_at_),
                             n_samples);
        }
      }
      {
        const std::scoped_lock lock(buffer_mutex_);
        pcm_buffer_.insert(pcm_buffer_.end(), samples, samples + n_samples);
      }
      ChunkCallback callback;
      {
        const std::scoped_lock lock(callback_mutex_);
        callback = chunk_callback_;
      }
      if (callback && n_samples > 0) {
        callback(std::span<const int16_t>(samples, n_samples));
      }
    }
  }

  pw_stream_queue_buffer(s, b);
}

void AudioCapture::onParamChanged(void* userdata, uint32_t id, const struct spa_pod* param) {
  (void)userdata;
  if (param == nullptr || id != SPA_PARAM_Format) {
    return;
  }

  struct spa_audio_info_raw info;
  if (spa_format_audio_raw_parse(param, &info) < 0) {
    return;
  }

  fprintf(stderr, "vinput: negotiated format: rate=%u channels=%u fmt=%d\n", info.rate,
          info.channels, info.format);
}

void AudioCapture::onStateChanged(void* userdata, enum pw_stream_state old,
                                  enum pw_stream_state state, const char* error) {
  (void)userdata;
  const char* err_str = (error != nullptr && error[0] != '\0') ? error : nullptr;
  vinput::debug::Log("capture stream state: %s -> %s%s%s\n", pw_stream_state_as_string(old),
                     pw_stream_state_as_string(state), err_str != nullptr ? " error=" : "",
                     err_str != nullptr ? err_str : "");
  if (state == PW_STREAM_STATE_ERROR) {
    fprintf(stderr, "vinput: PipeWire stream error: %s\n",
            err_str != nullptr ? err_str : "unknown error");
  }
}

bool AudioCapture::Start(std::string* error) {
  if (loop_) {
    return true;
  }

  loop_ = pw_thread_loop_new("vinput-capture-loop", nullptr);
  if (!loop_) {
    if (error) {
      *error = "failed to create PipeWire thread loop";
    }
    return false;
  }

  int ret = pw_thread_loop_start(loop_);
  if (ret < 0) {
    if (error) {
      *error = std::string("failed to start PipeWire thread loop: ") + strerror(-ret);
    }
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    return false;
  }

  return true;
}

std::string AudioCapture::CurrentTargetObject() const {
  std::lock_guard<std::mutex> lock(target_mutex_);
  return target_object_;
}

bool AudioCapture::CanReuseStreamLocked() const {
  if (!stream_) {
    return false;
  }
  const std::string wanted = CurrentTargetObject();
  // connected_target_object_ empty means "default" was used at connect time.
  if (wanted.empty() || wanted == "default") {
    return connected_target_object_.empty() || connected_target_object_ == "default";
  }
  return connected_target_object_ == wanted;
}

bool AudioCapture::CreateStream(bool start_inactive, std::string* error) {
  if (!loop_) {
    if (error) {
      *error = "audio capture loop is not initialized";
    }
    return false;
  }

  if (stream_) {
    return true;
  }

  stream_events_.version = PW_VERSION_STREAM_EVENTS;
  stream_events_.param_changed = onParamChanged;
  stream_events_.process = onProcess;
  stream_events_.state_changed = onStateChanged;

  std::string target_object = CurrentTargetObject();

  pw_thread_loop_lock(loop_);

  auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                       PW_KEY_MEDIA_ROLE, "Communication",
                                       PW_KEY_STREAM_CAPTURE_SINK, "false", nullptr);
  if (!properties) {
    if (error) {
      *error = "failed to allocate PipeWire properties";
    }
    pw_thread_loop_unlock(loop_);
    return false;
  }

  if (!target_object.empty() && target_object != "default") {
    pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target_object.c_str());
    fprintf(stderr, "vinput: using PipeWire target.object=%s\n", target_object.c_str());
  }

  stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "vinput-capture", properties,
                                 &stream_events_, this);

  if (!stream_) {
    if (error) {
      *error = "failed to create PipeWire stream";
    }
    pw_thread_loop_unlock(loop_);
    return false;
  }

  uint8_t pod_buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
  struct spa_audio_info_raw raw_info{};
  raw_info.format = SPA_AUDIO_FORMAT_S16_LE;
  raw_info.rate = 16000;
  raw_info.channels = 1;
  raw_info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const struct spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw_info);

  int flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;
  if (start_inactive) {
    flags |= PW_STREAM_FLAG_INACTIVE;
  }

  int ret = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(flags), params, 1);

  if (ret < 0) {
    if (error) {
      *error = std::string("failed to connect PipeWire stream: ") + strerror(-ret);
    }
    pw_stream_destroy(stream_);
    stream_ = nullptr;
    stream_active_ = false;
    pw_thread_loop_unlock(loop_);
    return false;
  }

  stream_active_ = !start_inactive;
  connected_target_object_ =
      (target_object.empty() || target_object == "default") ? std::string{} : target_object;
  pw_thread_loop_unlock(loop_);
  return true;
}

bool AudioCapture::SetStreamActive(bool active, std::string* error) {
  if (!loop_ || !stream_) {
    if (error) {
      *error = "audio capture stream is not initialized";
    }
    return false;
  }

  if (stream_active_ == active) {
    return true;
  }

  pw_thread_loop_lock(loop_);
  const int ret = pw_stream_set_active(stream_, active);
  if (ret < 0) {
    if (error) {
      *error = std::string("failed to set PipeWire stream active=") + (active ? "true" : "false") +
               ": " + strerror(-ret);
    }
    pw_thread_loop_unlock(loop_);
    return false;
  }
  stream_active_ = active;
  pw_thread_loop_unlock(loop_);

  if (!active) {
    MarkStreamDeactivated();
  }
  return true;
}

void AudioCapture::MarkStreamDestroyed() {
  std::lock_guard<std::mutex> lock(timing_mutex_);
  last_stream_destroyed_at_ = std::chrono::steady_clock::now();
  last_stream_deactivated_at_ = last_stream_destroyed_at_;
  idle_destroy_deadline_.reset();
  recording_armed_at_.reset();
  first_buffer_at_.reset();
}

void AudioCapture::MarkStreamDeactivated() {
  std::lock_guard<std::mutex> lock(timing_mutex_);
  last_stream_deactivated_at_ = std::chrono::steady_clock::now();
  recording_armed_at_.reset();
  first_buffer_at_.reset();
}

void AudioCapture::ScheduleIdleDestroy() {
  const long grace_ms = IdleDestroyMs();
  std::lock_guard<std::mutex> lock(timing_mutex_);
  if (grace_ms <= 0) {
    idle_destroy_deadline_ = std::chrono::steady_clock::now();
    return;
  }
  idle_destroy_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
  vinput::debug::Log("capture idle destroy scheduled in %ld ms\n", grace_ms);
}

void AudioCapture::CancelIdleDestroy() {
  std::lock_guard<std::mutex> lock(timing_mutex_);
  if (idle_destroy_deadline_.has_value()) {
    vinput::debug::Log("capture idle destroy cancelled\n");
  }
  idle_destroy_deadline_.reset();
}

bool AudioCapture::MaybeDestroyExpiredStream() {
  if (recording_.load(std::memory_order_relaxed)) {
    return false;
  }
  if (!stream_) {
    return false;
  }

  std::optional<std::chrono::steady_clock::time_point> deadline;
  {
    std::lock_guard<std::mutex> lock(timing_mutex_);
    deadline = idle_destroy_deadline_;
  }
  if (!deadline.has_value()) {
    return false;
  }
  if (std::chrono::steady_clock::now() < *deadline) {
    return false;
  }

  vinput::debug::Log("capture idle grace expired; destroying reusable stream\n");
  DestroyStream();
  return true;
}

void AudioCapture::DestroyStream() {
  recording_.store(false, std::memory_order_relaxed);
  if (!loop_ || !stream_) {
    stream_active_ = false;
    connected_target_object_.clear();
    {
      std::lock_guard<std::mutex> lock(timing_mutex_);
      idle_destroy_deadline_.reset();
    }
    return;
  }

  pw_thread_loop_lock(loop_);
  pw_stream_destroy(stream_);
  stream_ = nullptr;
  stream_active_ = false;
  connected_target_object_.clear();
  pw_thread_loop_unlock(loop_);
  MarkStreamDestroyed();
}

bool AudioCapture::BeginRecording(std::string* error, StartTiming* timing) {
  StartTiming local_timing;
  local_timing.reuse_policy_enabled = StreamReuseEnabled();
  CancelIdleDestroy();
  const auto begin_at = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(timing_mutex_);
    const auto idle_ref = last_stream_deactivated_at_.has_value() ? last_stream_deactivated_at_
                                                                  : last_stream_destroyed_at_;
    if (idle_ref.has_value()) {
      local_timing.idle_gap_ms = MillisecondsBetween(*idle_ref, begin_at);
    }
    first_buffer_at_.reset();
    recording_armed_at_.reset();
  }

  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    pcm_buffer_.clear();
  }

  const bool reuse = local_timing.reuse_policy_enabled && CanReuseStreamLocked();
  if (!reuse) {
    DestroyStream();
    local_timing.stream_reused = false;
    local_timing.created_new_stream = true;

    recording_.store(true, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(timing_mutex_);
      recording_armed_at_ = std::chrono::steady_clock::now();
    }

    const auto create_start = std::chrono::steady_clock::now();
    // When reuse policy is on, create inactive then activate so subsequent
    // stops can deactivate without tearing down negotiation.
    const bool start_inactive = local_timing.reuse_policy_enabled;
    if (!CreateStream(start_inactive, error)) {
      recording_.store(false, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(timing_mutex_);
      recording_armed_at_.reset();
      return false;
    }
    local_timing.create_stream_ms =
        MillisecondsBetween(create_start, std::chrono::steady_clock::now());

    if (start_inactive) {
      const auto active_start = std::chrono::steady_clock::now();
      if (!SetStreamActive(true, error)) {
        recording_.store(false, std::memory_order_relaxed);
        DestroyStream();
        return false;
      }
      local_timing.set_active_ms =
          MillisecondsBetween(active_start, std::chrono::steady_clock::now());
    } else {
      local_timing.set_active_ms = 0;
    }
  } else {
    local_timing.stream_reused = true;
    local_timing.created_new_stream = false;
    local_timing.create_stream_ms = 0;

    recording_.store(true, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(timing_mutex_);
      recording_armed_at_ = std::chrono::steady_clock::now();
    }

    const auto active_start = std::chrono::steady_clock::now();
    if (!SetStreamActive(true, error)) {
      // Reuse path failed — fall back to full recreate once.
      vinput::debug::Log("capture set_active(true) on reused stream failed (%s), recreating\n",
                         error && !error->empty() ? error->c_str() : "unknown");
      DestroyStream();
      local_timing.stream_reused = false;
      local_timing.created_new_stream = true;
      const auto create_start = std::chrono::steady_clock::now();
      if (!CreateStream(true, error)) {
        recording_.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(timing_mutex_);
        recording_armed_at_.reset();
        return false;
      }
      local_timing.create_stream_ms =
          MillisecondsBetween(create_start, std::chrono::steady_clock::now());
      if (!SetStreamActive(true, error)) {
        recording_.store(false, std::memory_order_relaxed);
        DestroyStream();
        return false;
      }
      local_timing.set_active_ms =
          MillisecondsBetween(active_start, std::chrono::steady_clock::now());
    } else {
      local_timing.set_active_ms =
          MillisecondsBetween(active_start, std::chrono::steady_clock::now());
    }
  }

  vinput::debug::Log("capture begin idle_gap_ms=%ld create_stream_ms=%ld set_active_ms=%ld "
                     "stream_reused=%d created_new_stream=%d reuse_policy=%d\n",
                     local_timing.idle_gap_ms, local_timing.create_stream_ms,
                     local_timing.set_active_ms, local_timing.stream_reused ? 1 : 0,
                     local_timing.created_new_stream ? 1 : 0,
                     local_timing.reuse_policy_enabled ? 1 : 0);

  if (timing) {
    *timing = local_timing;
  }
  return true;
}

void AudioCapture::EndRecording() {
  recording_.store(false, std::memory_order_relaxed);
  if (StreamReuseEnabled() && stream_) {
    std::string error;
    if (!SetStreamActive(false, &error)) {
      vinput::debug::Log("capture EndRecording set_active(false) failed: %s\n", error.c_str());
      DestroyStream();
      return;
    }
    if (IdleDestroyMs() <= 0) {
      DestroyStream();
    } else {
      ScheduleIdleDestroy();
    }
  }
}

std::vector<int16_t> AudioCapture::StopAndGetBuffer() {
  recording_.store(false, std::memory_order_relaxed);
  if (StreamReuseEnabled() && stream_) {
    std::string error;
    if (!SetStreamActive(false, &error)) {
      vinput::debug::Log("capture StopAndGetBuffer set_active(false) failed: %s; destroying\n",
                         error.c_str());
      DestroyStream();
    } else if (IdleDestroyMs() <= 0) {
      DestroyStream();
    } else {
      ScheduleIdleDestroy();
    }
  } else {
    DestroyStream();
  }
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  auto result = pcm_buffer_;
  pcm_buffer_.clear();
  return result;
}

void AudioCapture::Stop() {
  recording_.store(false, std::memory_order_relaxed);
  CancelIdleDestroy();
  DestroyStream();
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  pcm_buffer_.clear();
}

bool AudioCapture::IsRecording() const {
  return recording_.load(std::memory_order_relaxed);
}

std::optional<long> AudioCapture::FirstBufferLatencyMs() const {
  std::lock_guard<std::mutex> lock(timing_mutex_);
  if (!recording_armed_at_.has_value() || !first_buffer_at_.has_value()) {
    return std::nullopt;
  }
  return MillisecondsBetween(*recording_armed_at_, *first_buffer_at_);
}

void AudioCapture::SetTargetObject(std::string target_object) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(target_mutex_);
    if (target_object_ == target_object) {
      return;
    }
    target_object_ = std::move(target_object);
    changed = true;
  }
  if (!changed) {
    return;
  }
  // Target change invalidates a connected stream. Destroy when not recording so
  // the next BeginRecording reconnects to the new source.
  if (!recording_.load(std::memory_order_relaxed) && stream_) {
    vinput::debug::Log("capture target changed; destroying reusable stream for reconnect\n");
    DestroyStream();
  }
}

void AudioCapture::SetChunkCallback(ChunkCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  chunk_callback_ = std::move(callback);
}
