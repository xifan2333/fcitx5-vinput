#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/client/client_command.h"
#include "common/dbus/dbus_interface.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/audio_capture.h"
#include "daemon/audio/output_ducker.h"
#include "daemon/runtime/audio_recording_buffer.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/recognition_pipeline.h"
#include "daemon/runtime/runtime_state_machine.h"
#include "daemon/runtime/session_lifecycle_manager.h"

namespace vinput::daemon::remote {
class RemoteTextService;
}

namespace vinput::daemon::runtime {

struct RecognitionOrder {
  vinput::daemon::asr::AudioDeliveryMode audio_delivery_mode =
      vinput::daemon::asr::AudioDeliveryMode::Chunked;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  vinput::daemon::asr::BackendDescriptor backend;
  std::vector<int16_t> pcm;
  PostprocessRequest postprocess_request;
  bool ime = true;
};

class DaemonRuntimeController {
public:
  DaemonRuntimeController(AudioCapture* capture, DbusService* dbus,
                          vinput::daemon::asr::RecognitionSessionManager* recognition_manager,
                          RecognitionPipeline* pipeline,
                          vinput::daemon::remote::RemoteTextService* remote_text_service = nullptr);
  ~DaemonRuntimeController();

  DaemonRuntimeController(const DaemonRuntimeController&) = delete;
  DaemonRuntimeController& operator=(const DaemonRuntimeController&) = delete;
  DaemonRuntimeController(DaemonRuntimeController&&) = delete;
  DaemonRuntimeController& operator=(DaemonRuntimeController&&) = delete;

  DbusService::MethodResult StartRecording();
  DbusService::MethodResult StartCommandRecording(const std::string& selected_text);
  DbusService::MethodResult StopRecording(const std::string& scene_id);
  DbusService::MethodResult CancelOperation(bool commit_raw_text);

  // Extended session control supporting IME bypass, per-session capture device, and injection
  DbusService::MethodResult StartRecording(bool ime, bool is_command,
                                           const std::string& selected_text = {},
                                           const std::string& capture_device = {});
  DbusService::MethodResult StopRecording(bool ime, const std::string& scene_id);
  DbusService::MethodResult CancelOperation(bool ime, bool commit_raw_text);
  DbusService::MethodResult InjectText(bool ime, vinput::client::InjectStage stage,
                                       const std::string& text, const std::string& scene_id = {});

  void DispatchClientCommand(const vinput::client::ClientCommand& command);
  void DispatchClientLine(std::string_view line);

  DbusService::MethodResult ReloadAsrBackend();
  std::string GetStatus() const;
  vinput::dbus::AsrBackendState GetAsrBackendState() const;
  int GetNotifyFd() const;
  void FlushDeferredActions();

  void StartWorker();
  void Shutdown();

private:
  DbusService::MethodResult StartRecordingInternal(bool ime, bool is_command,
                                                   const std::string& selected_text,
                                                   const std::string& capture_device);
  void NotifyStatusChanged(vinput::dbus::Status status);
  bool SynchronizeAsrBackend(std::string* error = nullptr);
  void MaybeApplyPendingAsrBackendReload();
  void DrainIncomingAudio();
  void HandleIncomingAudio(std::span<const int16_t> pcm);
  void PollActiveSessionEvents();
  bool FailActiveSession(const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
                         const std::string& error_message);
  void ScheduleCaptureStopOnMainThread();
  void WakeMainThread() const;
  void RestoreOutputIfDucked();
  void ResetRecordingStateLocked();
  void EnterPostprocessing(bool command_mode);
  void ResetToIdle();
  void WorkerMain();

  AudioCapture* capture_ = nullptr;
  DbusService* dbus_ = nullptr;
  vinput::daemon::audio::OutputDucker output_ducker_;
  vinput::daemon::asr::RecognitionSessionManager* recognition_manager_ = nullptr;
  RecognitionPipeline* pipeline_ = nullptr;
  vinput::daemon::remote::RemoteTextService* remote_text_service_ = nullptr;

  mutable std::mutex state_mutex_;
  std::condition_variable worker_cv_;
  RuntimeStateMachine state_machine_;
  std::atomic<bool> postprocessing_cancel_requested_{false};
  std::optional<RecognitionOrder> current_order_;
  SessionLifecycleManager session_lifecycle_;
  vinput::daemon::asr::BackendDescriptor active_backend_;
  AudioRecordingBuffer audio_buffer_;
  std::string latest_final_text_;
  std::optional<std::chrono::steady_clock::time_point> recording_started_at_;
  std::optional<std::chrono::steady_clock::time_point> first_non_silent_at_;
  bool first_partial_logged_ = false;
  bool start_recording_in_progress_ = false;
  bool pending_asr_backend_reload_ = false;
  bool session_ime_ = true;
  int notify_fd_ = -1;
  std::atomic<bool> pending_capture_stop_{false};
  std::atomic<bool> accepting_chunks_{false};
  std::atomic<bool> worker_running_{false};
  std::thread worker_;
};

} // namespace vinput::daemon::runtime
