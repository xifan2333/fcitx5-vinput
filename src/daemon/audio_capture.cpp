#include "audio_capture.h"

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <cstdio>
#include <cstring>

AudioCapture::AudioCapture() { pw_init(nullptr, nullptr); }

AudioCapture::~AudioCapture() {
  DestroyStream();
  if (loop_) {
    pw_thread_loop_stop(loop_);
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
  pw_deinit();
}

void AudioCapture::onProcess(void *userdata) {
  auto *self = static_cast<AudioCapture *>(userdata);
  self->processCallback();
}

void AudioCapture::processCallback() {
  if (!stream_) {
    return;
  }

  struct pw_buffer *b = pw_stream_dequeue_buffer(stream_);
  if (!b) {
    return;
  }

  struct spa_buffer *buf = b->buffer;
  if (buf->datas[0].data == nullptr) {
    pw_stream_queue_buffer(stream_, b);
    return;
  }

  if (recording_.load(std::memory_order_relaxed)) {
    auto *samples = static_cast<int16_t *>(buf->datas[0].data);
    uint32_t n_samples = buf->datas[0].chunk->size / sizeof(int16_t);
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    pcm_buffer_.insert(pcm_buffer_.end(), samples, samples + n_samples);
  }

  pw_stream_queue_buffer(stream_, b);
}

void AudioCapture::onParamChanged(void *userdata, uint32_t id,
                                  const struct spa_pod *param) {
  (void)userdata;
  if (param == nullptr || id != SPA_PARAM_Format) {
    return;
  }

  struct spa_audio_info_raw info;
  if (spa_format_audio_raw_parse(param, &info) < 0) {
    return;
  }

  fprintf(stderr, "vinput: negotiated format: rate=%u channels=%u fmt=%d\n",
          info.rate, info.channels, info.format);
}

bool AudioCapture::Start() {
  if (loop_) {
    return true;
  }

  loop_ = pw_thread_loop_new("vinput-capture-loop", nullptr);
  if (!loop_) {
    fprintf(stderr, "vinput: failed to create PipeWire thread loop\n");
    return false;
  }

  int ret = pw_thread_loop_start(loop_);
  if (ret < 0) {
    fprintf(stderr, "vinput: failed to start PipeWire thread loop: %s\n",
            strerror(-ret));
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    return false;
  }

  return true;
}

bool AudioCapture::CreateStream() {
  if (!loop_) {
    return false;
  }

  if (stream_) {
    return true;
  }

  stream_events_.version = PW_VERSION_STREAM_EVENTS;
  stream_events_.param_changed = onParamChanged;
  stream_events_.process = onProcess;

  std::string target_object;
  {
    std::lock_guard<std::mutex> lock(target_mutex_);
    target_object = target_object_;
  }

  pw_thread_loop_lock(loop_);

  auto *properties =
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                        "Capture", PW_KEY_MEDIA_ROLE, "Communication", nullptr);
  if (!properties) {
    fprintf(stderr, "vinput: failed to allocate PipeWire properties\n");
    pw_thread_loop_unlock(loop_);
    return false;
  }

  if (!target_object.empty()) {
    pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target_object.c_str());
    fprintf(stderr, "vinput: using PipeWire target.object=%s\n",
            target_object.c_str());
  }

  stream_ =
      pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "vinput-capture",
                           properties, &stream_events_, this);

  if (!stream_) {
    fprintf(stderr, "vinput: failed to create PipeWire stream\n");
    pw_thread_loop_unlock(loop_);
    return false;
  }

  uint8_t pod_buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
  struct spa_audio_info_raw raw_info{};
  raw_info.format = SPA_AUDIO_FORMAT_S16_LE;
  raw_info.rate = 16000;
  raw_info.channels = 1;
  const struct spa_pod *params[1];
  params[0] =
      spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw_info);

  int ret = pw_stream_connect(
      stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS),
      params, 1);

  if (ret < 0) {
    fprintf(stderr, "vinput: failed to connect PipeWire stream: %s\n",
            strerror(-ret));
    pw_stream_destroy(stream_);
    stream_ = nullptr;
    pw_thread_loop_unlock(loop_);
    return false;
  }

  pw_thread_loop_unlock(loop_);
  return true;
}

void AudioCapture::DestroyStream() {
  recording_.store(false, std::memory_order_relaxed);
  if (!loop_ || !stream_) {
    return;
  }

  pw_thread_loop_lock(loop_);
  pw_stream_destroy(stream_);
  stream_ = nullptr;
  pw_thread_loop_unlock(loop_);
}

bool AudioCapture::BeginRecording() {
  DestroyStream();
  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    pcm_buffer_.clear();
  }
  recording_.store(true, std::memory_order_relaxed);
  if (!CreateStream()) {
    recording_.store(false, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void AudioCapture::EndRecording() {
  recording_.store(false, std::memory_order_relaxed);
}

std::vector<int16_t> AudioCapture::StopAndGetBuffer() {
  recording_.store(false, std::memory_order_relaxed);
  DestroyStream();
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  auto result = pcm_buffer_;
  pcm_buffer_.clear();
  return result;
}

bool AudioCapture::IsRecording() const {
  return recording_.load(std::memory_order_relaxed);
}

void AudioCapture::SetTargetObject(std::string target_object) {
  std::lock_guard<std::mutex> lock(target_mutex_);
  target_object_ = std::move(target_object);
}
