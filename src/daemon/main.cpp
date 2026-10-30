#include <atomic>
#include <cerrno>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <sys/poll.h>

extern "C" {
#include <signal.h>
}

#include "common/config/core_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/llm/adapter_manager.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/debug_log.h"

#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/audio_capture.h"
#include "daemon/postprocess/post_processor.h"
#include "daemon/remote/remote_text_service.h"
#include "daemon/runtime/daemon_runtime_controller.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/recognition_pipeline.h"

namespace {

std::atomic<bool>& RunningFlag() {
  static std::atomic<bool> running{true};
  return running;
}

void signal_handler(int sig) {
  (void)sig;
  RunningFlag() = false;
}

} // namespace

int main(int argc, char* argv[]) {
  vinput::i18n::Init();
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  if (argc > 0 && argv != nullptr) {
    const std::span<char* const> args(argv, static_cast<size_t>(argc));
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == nullptr) {
        continue;
      }
      const std::string_view arg(args[i]);
      if (arg.starts_with("--log-level=")) {
        vinput::log::SetLogLevel(arg.substr(12));
      } else if (arg == "--log-level" && i + 1 < args.size() && args[i + 1] != nullptr) {
        vinput::log::SetLogLevel(args[++i]);
      }
    }
  }

  DbusService dbus;
  vinput::notify::NotificationBridge notification_bridge(
      [&dbus](const auto& n) { dbus.EmitDesktopNotification(n); });

  auto startup_settings = LoadCoreConfig();
  NormalizeCoreConfig(&startup_settings);
  vinput::daemon::asr::RecognitionSessionManager recognition_manager(&notification_bridge);

  std::string asr_disabled_reason;
  if (!recognition_manager.Initialize(startup_settings, &asr_disabled_reason)) {
    if (asr_disabled_reason.empty()) {
      VLOG_WARN("running with ASR disabled");
    } else {
      VLOG_WARN("running with ASR disabled ({})", asr_disabled_reason);
    }
  }

  AudioCapture capture;
  std::string capture_error;
  if (!capture.Start(&capture_error)) {
    if (capture_error.empty()) {
      capture_error = "audio capture start failed";
    }
    VLOG_ERROR("{}", capture_error);
    return 1;
  }

  PostProcessor post_processor;

  using vinput::dbus::Status;
  using vinput::dbus::StatusToString;

  // --- Single-slot state (all protected by state_mutex) ---
  vinput::daemon::remote::RemoteTextService remote_text_service;
  vinput::daemon::runtime::RecognitionPipeline recognition_pipeline(&post_processor);
  vinput::daemon::runtime::DaemonRuntimeController runtime_controller(
      &capture, &dbus, &recognition_manager, &recognition_pipeline, &remote_text_service);

  dbus.SetStartHandler([&]() { return runtime_controller.StartRecording(); });

  dbus.SetStartCommandHandler([&](const std::string& selected_text) {
    return runtime_controller.StartCommandRecording(selected_text);
  });

  dbus.SetStopHandler([&](const std::string& scene_id) -> DbusService::MethodResult {
    return runtime_controller.StopRecording(scene_id);
  });

  dbus.SetCancelOperationHandler(
      [&](bool commit_raw_text) { return runtime_controller.CancelOperation(commit_raw_text); });

  dbus.SetStatusHandler([&]() -> std::string { return runtime_controller.GetStatus(); });
  dbus.SetAsrBackendStateHandler(
      [&]() -> vinput::dbus::AsrBackendState { return runtime_controller.GetAsrBackendState(); });
  dbus.SetReloadAsrBackendHandler([&]() { return runtime_controller.ReloadAsrBackend(); });

  dbus.SetStartAdapterHandler([&](const std::string& adapter_id) -> DbusService::MethodResult {
    std::string error;
    if (!vinput::adapter::Start(adapter_id, &notification_bridge, &error)) {
      return DbusService::MethodResult::Failure(error.empty() ? "failed to start adapter" : error);
    }
    return DbusService::MethodResult::Success();
  });

  dbus.SetStopAdapterHandler([&](const std::string& adapter_id) -> DbusService::MethodResult {
    std::string error;
    if (!vinput::adapter::Stop(adapter_id, &error, &notification_bridge)) {
      return DbusService::MethodResult::Failure(error.empty() ? "failed to stop adapter" : error);
    }
    return DbusService::MethodResult::Success();
  });

  std::string dbus_error;
  if (!dbus.Start(&dbus_error)) {
    if (dbus_error.empty()) {
      dbus_error = "DBus service start failed";
    }
    VLOG_ERROR("{}", dbus_error);
    return 1;
  }

  vinput::adapter::AutoStart(startup_settings, &notification_bridge);

  runtime_controller.StartWorker();
  std::string remote_error;
  if (!remote_text_service.Synchronize(startup_settings, &remote_error)) {
    if (remote_error.empty()) {
      VLOG_WARN("remote ASR service disabled");
    } else {
      VLOG_WARN("remote ASR service disabled ({})", remote_error);
    }
  }

  VLOG_INFO("running");

  const int dbus_fd = dbus.GetFd();
  const int notify_fd = dbus.GetNotifyFd();
  const int runtime_notify_fd = runtime_controller.GetNotifyFd();
  while (RunningFlag()) {
    pollfd fds[3] = {
        {.fd = dbus_fd, .events = POLLIN, .revents = 0},
        {.fd = notify_fd, .events = POLLIN, .revents = 0},
        {.fd = runtime_notify_fd, .events = POLLIN, .revents = 0},
    };

    const int ret = poll(fds, 3, 1000);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      VLOG_ERROR("poll error: {}", strerror(errno));
      break;
    }

    // Bounded warm grace: destroy inactive reusable streams after idle timeout.
    capture.MaybeDestroyExpiredStream();

    if (ret > 0) {
      if ((fds[1].revents & POLLIN) != 0) {
        dbus.FlushEmitQueue();
      }
      if ((fds[2].revents & POLLIN) != 0) {
        runtime_controller.FlushDeferredActions();
      }
      if ((fds[0].revents & POLLIN) != 0) {
        while (dbus.ProcessOnce()) {
          // process all pending messages
        }
      }
    }
  }

  VLOG_INFO("shutting down");
  post_processor.Shutdown();
  remote_text_service.Shutdown();
  runtime_controller.Shutdown();
  recognition_manager.Shutdown();
  vinput::adapter::Shutdown();
  return 0;
}
