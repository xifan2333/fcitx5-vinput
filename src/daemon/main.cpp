#include "asr_engine.h"
#include "audio_capture.h"
#include "common/core_config.h"
#include "common/dbus_interface.h"
#include "common/recognition_result.h"
#include "dbus_service.h"
#include "model_manager.h"
#include "post_processor.h"

#include <poll.h>
#include <signal.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
  (void)sig;
  g_running = false;
}

int main(int argc, char *argv[]) {
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  bool disable_asr = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-asr") == 0) {
      disable_asr = true;
    }
  }

  auto startup_settings = LoadCoreConfig();
  NormalizeCoreConfig(&startup_settings);
  ModelManager model_mgr(ResolveModelBaseDir(startup_settings.core).string(),
                         startup_settings.core.activeModel);
  const auto model_info = model_mgr.GetModelInfo();

  if (!disable_asr && !model_mgr.EnsureModels()) {
    fprintf(stderr, "vinput-daemon: model check failed, exiting\n");
    return 1;
  }

  if (!disable_asr) {
    fprintf(stderr, "vinput-daemon: using model '%s' from %s\n",
            model_mgr.GetModelName().c_str(), model_info.model.c_str());
    fprintf(stderr, "vinput-daemon: using tokens from %s\n",
            model_info.tokens.c_str());
  }

  AsrEngine asr;
  if (!disable_asr && !asr.Init(model_info, 4)) {
    fprintf(stderr, "vinput-daemon: ASR engine init failed, exiting\n");
    return 1;
  }
  if (disable_asr) {
    fprintf(stderr, "vinput-daemon: running with ASR disabled\n");
  }

  AudioCapture capture;
  if (!capture.Start()) {
    fprintf(stderr, "vinput-daemon: audio capture start failed, exiting\n");
    return 1;
  }

  DbusService dbus;
  PostProcessor post_processor;

  using vinput::dbus::Status;
  using vinput::dbus::StatusToString;

  std::atomic<Status> current_status{Status::Idle};

  dbus.SetStartHandler([&]() {
    auto runtime_settings = LoadCoreConfig();
    capture.SetTargetObject(runtime_settings.core.captureDevice);
    if (!capture.BeginRecording()) {
      current_status = Status::Error;
      dbus.EmitStatusChanged(StatusToString(Status::Error));
      fprintf(stderr, "vinput-daemon: failed to start recording\n");
      return;
    }
    current_status = Status::Recording;
    dbus.EmitStatusChanged(StatusToString(Status::Recording));
    fprintf(stderr, "vinput-daemon: recording started\n");
  });

  dbus.SetStopHandler([&](const std::string &scene_id) -> std::string {
    capture.EndRecording();
    auto pcm = capture.StopAndGetBuffer();
    if (pcm.empty()) {
      fprintf(stderr,
              "vinput-daemon: recording stopped with empty audio buffer\n");
      current_status = Status::Idle;
      dbus.EmitStatusChanged(StatusToString(Status::Idle));
      return "";
    }

    if (pcm.size() < AsrEngine::kMinSamplesForInference) {
      fprintf(stderr,
              "vinput-daemon: recording too short, skipping inference: "
              "%zu samples (%.1f ms)\n",
              pcm.size(), static_cast<double>(pcm.size()) * 1000.0 / 16000.0);
      current_status = Status::Idle;
      dbus.EmitStatusChanged(StatusToString(Status::Idle));
      return "";
    }

    current_status = Status::Inferring;
    dbus.EmitStatusChanged(StatusToString(Status::Inferring));
    fprintf(stderr, "vinput-daemon: recording stopped, starting inference\n");

    std::string text;
    if (!disable_asr) {
      text = asr.Infer(pcm);
    }

    auto result = vinput::result::PlainTextPayload(text);
    if (!text.empty()) {
      auto runtime_settings = LoadCoreConfig();
      NormalizeCoreConfig(&runtime_settings);
      const auto scene_config = vinput::scene::LoadConfig();
      const auto &scene = vinput::scene::Resolve(scene_config, scene_id);
      if (runtime_settings.llm.enabled && scene.llm) {
        current_status = Status::Postprocessing;
        dbus.EmitStatusChanged(StatusToString(Status::Postprocessing));
      }
      result = post_processor.Process(text, scene, runtime_settings);
      text = result.commitText;
    }

    if (!text.empty()) {
      dbus.EmitRecognitionResult(vinput::result::Serialize(result));
    }

    current_status = Status::Idle;
    dbus.EmitStatusChanged(StatusToString(Status::Idle));
    fprintf(stderr, "vinput-daemon: inference complete: %s\n", text.c_str());
    return text;
  });

  dbus.SetStatusHandler(
      [&]() -> std::string { return StatusToString(current_status.load()); });

  if (!dbus.Start()) {
    fprintf(stderr, "vinput-daemon: DBus service start failed, exiting\n");
    return 1;
  }

  fprintf(stderr, "vinput-daemon: running\n");

  int dbus_fd = dbus.GetFd();
  while (g_running) {
    struct pollfd fds[1];
    fds[0].fd = dbus_fd;
    fds[0].events = POLLIN;

    int ret = poll(fds, 1, 1000);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "vinput-daemon: poll error: %s\n", strerror(errno));
      break;
    }

    if (ret > 0 && (fds[0].revents & POLLIN)) {
      while (dbus.ProcessOnce()) {
        // process all pending messages
      }
    }
  }

  fprintf(stderr, "vinput-daemon: shutting down\n");
  asr.Shutdown();
  return 0;
}
