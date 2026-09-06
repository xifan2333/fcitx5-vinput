#define _GNU_SOURCE
#include <pipewire/pipewire.h>
#include <pthread.h>
#include <spa/param/audio/format-utils.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct CaptureData {
  struct pw_main_loop* loop;
  struct pw_stream* stream;
  uint64_t bytes;
  int32_t peak;
};

static void OnProcess(void* userdata) {
  struct CaptureData* data = userdata;
  struct pw_buffer* pw_buf = pw_stream_dequeue_buffer(data->stream);
  if (pw_buf == NULL) {
    return;
  }
  struct spa_buffer* buf = pw_buf->buffer;
  uint8_t* bytes = buf->datas[0].data;
  uint32_t size = buf->datas[0].chunk ? buf->datas[0].chunk->size : 0;
  if (bytes != NULL && size > 0) {
    data->bytes += size;
    const int16_t* samples = (const int16_t*)bytes;
    uint32_t count = size / sizeof(int16_t);
    for (uint32_t i = 0; i < count; ++i) {
      int32_t value = samples[i];
      if (value < 0) {
        value = -value;
      }
      if (value > data->peak) {
        data->peak = value;
      }
    }
  }
  pw_stream_queue_buffer(data->stream, pw_buf);
}

static void OnStateChanged(void* userdata, enum pw_stream_state old, enum pw_stream_state state,
                           const char* error) {
  (void)userdata;
  fprintf(stderr, "state %s -> %s%s%s\n", pw_stream_state_as_string(old),
          pw_stream_state_as_string(state), error ? " " : "", error ? error : "");
}

static const struct pw_stream_events kEvents = {
    PW_VERSION_STREAM_EVENTS,
    .process = OnProcess,
    .state_changed = OnStateChanged,
};

static void* QuitAfter(void* arg) {
  struct CaptureData* data = arg;
  sleep(5);
  pw_main_loop_quit(data->loop);
  return NULL;
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "unknown";
  const int use_mono = strcmp(mode, "mono") == 0 || strcmp(mode, "imono") == 0;
  const int use_inactive = mode[0] == 'i';
  const char* target = argc > 2 ? argv[2] : NULL;

  pw_init(&argc, &argv);
  struct CaptureData data = {0};
  data.loop = pw_main_loop_new(NULL);

  struct pw_properties* properties =
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                        PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_STREAM_CAPTURE_SINK, "false",
                        NULL);
  if (target != NULL && target[0] != '\0') {
    pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target);
    fprintf(stderr, "target.object=%s\n", target);
  }
  fprintf(stderr, "mode=%s\n", mode);

  data.stream = pw_stream_new_simple(pw_main_loop_get_loop(data.loop), "vinput-repro", properties,
                                     &kEvents, &data);

  uint8_t pod_buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
  struct spa_audio_info_raw raw_info = {0};
  raw_info.format = SPA_AUDIO_FORMAT_S16_LE;
  raw_info.rate = 16000;
  raw_info.channels = 1;
  if (use_mono) {
    raw_info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  }
  const struct spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw_info);

  int flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;
  if (use_inactive) {
    flags |= PW_STREAM_FLAG_INACTIVE;
  }
  int ret = pw_stream_connect(data.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                              (enum pw_stream_flags)flags, params, 1);
  if (ret < 0) {
    fprintf(stderr, "pw_stream_connect failed: %s\n", strerror(-ret));
    return 1;
  }

  if (use_inactive) {
    fprintf(stderr, "(inactive) waiting 1s before set_active(true)\n");
    sleep(1);
    ret = pw_stream_set_active(data.stream, true);
    fprintf(stderr, "(inactive) set_active ret=%d\n", ret);
  }

  pthread_t thread;
  pthread_create(&thread, NULL, QuitAfter, &data);
  pw_main_loop_run(data.loop);
  pthread_join(thread, NULL);

  printf("bytes=%llu peak=%d\n", (unsigned long long)data.bytes, data.peak);

  pw_stream_destroy(data.stream);
  pw_main_loop_destroy(data.loop);
  pw_deinit();
  return 0;
}
