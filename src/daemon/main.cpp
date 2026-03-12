#include "asr_engine.h"
#include "audio_capture.h"
#include "common/core_config.h"
#include "common/dbus_interface.h"
#include "common/i18n.h"
#include "common/model_manager.h"
#include "common/recognition_result.h"
#include "dbus_service.h"
#include "post_processor.h"

#include <poll.h>
#include <signal.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
  (void)sig;
  g_running = false;
}

int main(int argc, char *argv[]) {
  vinput::i18n::Init();
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
  ModelManager model_mgr(ResolveModelBaseDir(startup_settings).string(),
                         startup_settings.activeModel);
  auto model_info = model_mgr.GetModelInfo();

  if (!disable_asr && !model_mgr.EnsureModels()) {
    fprintf(stderr, "vinput-daemon: model check failed, exiting\n");
    return 1;
  }

  if (!disable_asr) {
    fprintf(stderr, "vinput-daemon: using model '%s' (type: %s, lang: %s)\n",
            model_mgr.GetModelName().c_str(), model_info.model_type.c_str(),
            startup_settings.defaultLanguage.c_str());
  }

  AsrEngine asr;
  if (!disable_asr) {
    AsrConfig asr_config;
    asr_config.language = startup_settings.defaultLanguage;
    asr_config.hotwords_file = startup_settings.hotwordsFile;
    if (!asr.Init(model_info, asr_config)) {
      fprintf(stderr, "vinput-daemon: ASR engine init failed, exiting\n");
      return 1;
    }
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

  struct InferenceJob {
    std::vector<int16_t> pcm;
    std::string scene_id;
    bool is_command = false;
    std::string selected_text;
  };

  std::mutex job_mutex;
  std::condition_variable job_cv;
  std::deque<InferenceJob> jobs;
  std::atomic<bool> worker_running{true};

  std::thread worker([&]() {
    while (worker_running) {
      InferenceJob job;
      {
        std::unique_lock<std::mutex> lock(job_mutex);
        job_cv.wait(lock,
                    [&]() { return !jobs.empty() || !worker_running.load(); });
        if (!worker_running && jobs.empty()) {
          break;
        }
        job = std::move(jobs.front());
        jobs.pop_front();
      }

      current_status = Status::Inferring;
      dbus.EmitStatusChanged(StatusToString(Status::Inferring));

      std::string text;
      if (!disable_asr) {
        text = asr.Infer(job.pcm);
      }

      auto result = vinput::result::PlainTextPayload(text);
      if (!text.empty()) {
        auto runtime_settings = LoadCoreConfig();
        NormalizeCoreConfig(&runtime_settings);
        vinput::scene::Config scene_config;
        scene_config.activeSceneId = runtime_settings.scenes.activeScene;
        scene_config.scenes = runtime_settings.scenes.definitions;
        if (job.is_command) {
          if (runtime_settings.llm.enabled) {
            current_status = Status::Postprocessing;
            dbus.EmitStatusChanged(StatusToString(Status::Postprocessing));
          }
          std::string llm_error;
          result = post_processor.ProcessCommand(text, job.selected_text,
                                                 runtime_settings, &llm_error);
          if (!llm_error.empty()) {
            dbus.EmitLlmError(llm_error);
          }
          text = result.commitText;
        } else {
          const auto &scene =
              vinput::scene::Resolve(scene_config, job.scene_id);
          if (runtime_settings.llm.enabled && !scene.prompt.empty()) {
            current_status = Status::Postprocessing;
            dbus.EmitStatusChanged(StatusToString(Status::Postprocessing));
          }
          std::string llm_error;
          result = post_processor.Process(text, scene, runtime_settings,
                                          &llm_error);
          if (!llm_error.empty()) {
            dbus.EmitLlmError(llm_error);
          }
          text = result.commitText;
        }
      }

      if (!text.empty()) {
        dbus.EmitRecognitionResult(vinput::result::Serialize(result));
      }

      bool has_more = false;
      {
        std::lock_guard<std::mutex> lock(job_mutex);
        has_more = !jobs.empty();
      }
      if (!has_more) {
        current_status = Status::Idle;
        dbus.EmitStatusChanged(StatusToString(Status::Idle));
      } else {
        current_status = Status::Inferring;
        dbus.EmitStatusChanged(StatusToString(Status::Inferring));
      }
    }
  });

  bool current_is_command = false;
  std::string current_selected_text;

  dbus.SetStartHandler([&]() {
    current_is_command = false;
    current_selected_text.clear();
    auto runtime_settings = LoadCoreConfig();
    capture.SetTargetObject(runtime_settings.captureDevice);
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

  dbus.SetStartCommandHandler([&](const std::string &selected_text) {
    current_is_command = true;
    current_selected_text = selected_text;
    auto runtime_settings = LoadCoreConfig();
    capture.SetTargetObject(runtime_settings.captureDevice);
    if (!capture.BeginRecording()) {
      current_status = Status::Error;
      dbus.EmitStatusChanged(StatusToString(Status::Error));
      fprintf(stderr, "vinput-daemon: failed to start command recording\n");
      return;
    }
    current_status = Status::Recording;
    dbus.EmitStatusChanged(StatusToString(Status::Recording));
    fprintf(stderr,
            "vinput-daemon: command recording started (selected_text length: "
            "%zu chars)\n",
            selected_text.size());
  });

  dbus.SetStopHandler([&](const std::string &scene_id) -> std::string {
    capture.EndRecording();
    auto pcm = capture.StopAndGetBuffer();
    if (pcm.empty()) {
      fprintf(stderr,
              "vinput-daemon: recording stopped with empty audio buffer\n");
      current_is_command = false;
      current_selected_text.clear();
      current_status = Status::Idle;
      dbus.EmitStatusChanged(StatusToString(Status::Idle));
      return "";
    }

    if (pcm.size() < AsrEngine::kMinSamplesForInference) {
      fprintf(stderr,
              "vinput-daemon: recording too short, skipping inference: "
              "%zu samples (%.1f ms)\n",
              pcm.size(), static_cast<double>(pcm.size()) * 1000.0 / 16000.0);
      current_is_command = false;
      current_selected_text.clear();
      current_status = Status::Idle;
      dbus.EmitStatusChanged(StatusToString(Status::Idle));
      return "";
    }

    {
      std::lock_guard<std::mutex> lock(job_mutex);
      jobs.push_back({std::move(pcm), scene_id, current_is_command,
                      current_selected_text});
    }
    current_is_command = false;
    current_selected_text.clear();
    current_status = Status::Inferring;
    dbus.EmitStatusChanged(StatusToString(Status::Inferring));
    job_cv.notify_one();
    fprintf(stderr, "vinput-daemon: recording stopped, queued inference\n");
    return "";
  });

  dbus.SetStatusHandler(
      [&]() -> std::string { return StatusToString(current_status.load()); });

  if (!dbus.Start()) {
    fprintf(stderr, "vinput-daemon: DBus service start failed, exiting\n");
    return 1;
  }

  fprintf(stderr, "vinput-daemon: running\n");

  int dbus_fd = dbus.GetFd();
  int notify_fd = dbus.GetNotifyFd();
  while (g_running) {
    struct pollfd fds[2];
    fds[0].fd = dbus_fd;
    fds[0].events = POLLIN;
    fds[1].fd = notify_fd;
    fds[1].events = POLLIN;

    int ret = poll(fds, 2, 1000);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "vinput-daemon: poll error: %s\n", strerror(errno));
      break;
    }

    if (ret > 0) {
      if (fds[1].revents & POLLIN) {
        dbus.FlushEmitQueue();
      }
      if (fds[0].revents & POLLIN) {
        while (dbus.ProcessOnce()) {
          // process all pending messages
        }
      }
    }
  }

  fprintf(stderr, "vinput-daemon: shutting down\n");
  worker_running = false;
  job_cv.notify_all();
  if (worker.joinable()) {
    worker.join();
  }
  asr.Shutdown();
  return 0;
}
