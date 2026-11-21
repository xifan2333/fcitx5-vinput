#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <vector>

extern "C" {
#include <signal.h>
}

#include "common/client/client_manager.h"
#include "common/config/core_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/debug_log.h"

#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/audio_capture.h"
#include "daemon/postprocess/post_processor.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/lane_supervisor.h"
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

void ParseCommandLineLogLevel(int argc, char* argv[]) {
  if (argc <= 0 || argv == nullptr) {
    return;
  }
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

void WireDbusHandlers(DbusService& dbus, vinput::daemon::runtime::LaneSupervisor& supervisor,
                      vinput::notify::NotificationBridge& notification_bridge) {
  dbus.SetStartHandler([&]() { return supervisor.StartRecording(); });

  dbus.SetStartCommandHandler([&](const std::string& selected_text) {
    return supervisor.StartCommandRecording(selected_text);
  });

  dbus.SetStopHandler([&](const std::string& scene_id) -> DbusService::MethodResult {
    return supervisor.StopRecording(scene_id);
  });

  dbus.SetCancelOperationHandler(
      [&](bool commit_raw_text) { return supervisor.CancelOperation(commit_raw_text); });

  dbus.SetStatusHandler([&]() -> std::string { return supervisor.GetStatus(); });
  dbus.SetAsrBackendStateHandler(
      [&]() -> vinput::dbus::AsrBackendState { return supervisor.GetAsrBackendState(); });
  dbus.SetReloadAsrBackendHandler([&]() { return supervisor.ReloadAsrBackend(); });

  dbus.SetTranscribeStreamHandler([&](int fd, const std::string& scene_id, bool ime) {
    return supervisor.TranscribeStream(fd, scene_id, ime);
  });

  dbus.SetStartClientHandler([&](const std::string& client_id) -> DbusService::MethodResult {
    std::string error;
    if (!vinput::client::Start(
            client_id, &notification_bridge,
            [&](std::string_view cid, std::string_view line) {
              supervisor.EnqueueClientLine(cid, line);
            },
            &error)) {
      return DbusService::MethodResult::Failure(error.empty() ? _("failed to start client")
                                                              : error);
    }
    return DbusService::MethodResult::Success();
  });

  dbus.SetStopClientHandler([&](const std::string& client_id) -> DbusService::MethodResult {
    supervisor.ReclaimLane(client_id);
    std::string error;
    if (!vinput::client::Stop(client_id, &error, &notification_bridge)) {
      return DbusService::MethodResult::Failure(error.empty() ? "failed to stop client" : error);
    }
    return DbusService::MethodResult::Success();
  });

  dbus.SetGetClientStatusesHandler([&]() -> std::vector<vinput::dbus::ClientStatus> {
    return vinput::client::GetClientStatuses();
  });
}

void DispatchPollEvents(const pollfd* fds, DbusService& dbus,
                        vinput::daemon::runtime::LaneSupervisor& supervisor) {
  if ((fds[1].revents & POLLIN) != 0) {
    dbus.FlushEmitQueue();
  }
  if ((fds[2].revents & POLLIN) != 0) {
    supervisor.FlushDeferredActions();
  }
  if ((fds[0].revents & POLLIN) != 0) {
    while (dbus.ProcessOnce()) {
      // process all pending messages
    }
  }
}

void RunEventLoop(DbusService& dbus, vinput::daemon::runtime::LaneSupervisor& supervisor) {
  const int dbus_fd = dbus.GetFd();
  const int notify_fd = dbus.GetNotifyFd();
  const int runtime_notify_fd = supervisor.GetNotifyFd();
  while (RunningFlag()) {
    pollfd fds[3] = {
        {.fd = dbus_fd, .events = POLLIN, .revents = 0},
        {.fd = notify_fd, .events = POLLIN, .revents = 0},
        {.fd = runtime_notify_fd, .events = POLLIN, .revents = 0},
    };

    const int ret = poll(fds, 3, supervisor.NextTimeoutMs());
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      VLOG_ERROR("poll error: {}", strerror(errno));
      break;
    }

    if (ret == 0) {
      supervisor.FlushDeferredActions();
    } else {
      DispatchPollEvents(fds, dbus, supervisor);
    }
  }
}

} // namespace

int main(int argc, char* argv[]) {
  vinput::i18n::Init();
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  ParseCommandLineLogLevel(argc, argv);

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

  PostProcessor post_processor(&notification_bridge);

  vinput::daemon::runtime::RecognitionPipeline recognition_pipeline(&post_processor);
  vinput::daemon::runtime::LaneSupervisor supervisor(&capture, &dbus, &recognition_manager,
                                                     &recognition_pipeline);

  WireDbusHandlers(dbus, supervisor, notification_bridge);

  std::string dbus_error;
  if (!dbus.Start(&dbus_error)) {
    if (dbus_error.empty()) {
      dbus_error = "DBus service start failed";
    }
    VLOG_ERROR("{}", dbus_error);
    return 1;
  }

  vinput::client::AutoStart(startup_settings, &notification_bridge,
                            [&](std::string_view cid, std::string_view line) {
                              supervisor.EnqueueClientLine(cid, line);
                            });

  VLOG_INFO("running");
  RunEventLoop(dbus, supervisor);

  VLOG_INFO("shutting down");
  vinput::client::Shutdown();
  post_processor.Shutdown();
  supervisor.Shutdown();
  recognition_manager.Shutdown();
  return 0;
}
