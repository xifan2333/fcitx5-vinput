#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/dbus/dbus_interface.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/audio_capture.h"
#include "daemon/audio/output_ducker.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/recognition_pipeline.h"

namespace vinput::daemon::remote {
class RemoteTextService;
}

namespace vinput::daemon::runtime {

class DaemonRuntimeController {
public:
  DaemonRuntimeController(AudioCapture* capture, DbusService* dbus,
                          vinput::daemon::asr::RecognitionSessionManager* recognition_manager,
                          RecognitionPipeline* pipeline,
                          vinput::daemon::remote::RemoteTextService* remote_text_service = nullptr);
  ~DaemonRuntimeController();

  DbusService::MethodResult StartRecording();
  DbusService::MethodResult StartCommandRecording(const std::string& selected_text);
  DbusService::MethodResult StopRecording(const std::string& scene_id);
  DbusService::MethodResult CancelPostprocessing(bool commit_raw_text);
  DbusService::MethodResult ReloadAsrBackend();
  std::string GetStatus() const;
  vinput::dbus::AsrBackendState GetAsrBackendState() const;
  int GetNotifyFd() const;
  void FlushDeferredActions();

  void StartWorker();
  void Shutdown();

private:
  enum class PostprocessingState : std::uint8_t {
    Inactive,
    Dictation,
    Command,
    Discard,
    CommitRaw
  };

  DbusService::MethodResult StartRecordingInternal(bool is_command,
                                                   const std::string& selected_text);
  bool SynchronizeAsrBackend(std::string* error = nullptr);
  void MaybeApplyPendingAsrBackendReload();
  void HandleIncomingAudio(std::span<const int16_t> pcm);
  void ScheduleCaptureStopOnMainThread();
  void RestoreOutputIfDucked();
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> ReleaseActiveSessionLocked();
  void EnterPostprocessing(bool command_mode);
  PostprocessingState TakePostprocessingState();
  void ResetToIdle();
  void WorkerMain();

  AudioCapture* capture_ = nullptr;
  DbusService* dbus_ = nullptr;
  vinput::daemon::audio::OutputDucker output_ducker_;
  vinput::daemon::asr::RecognitionSessionManager* recognition_manager_ = nullptr;
  RecognitionPipeline* pipeline_ = nullptr;
  vinput::daemon::remote::RemoteTextService* remote_text_service_ = nullptr;

  mutable std::mutex state_mutex_;
  mutable std::mutex session_io_mutex_;
  std::condition_variable worker_cv_;
  vinput::dbus::Status phase_ = vinput::dbus::Status::Idle;
  PostprocessingState postprocessing_state_ = PostprocessingState::Inactive;
  std::atomic<bool> postprocessing_cancel_requested_{false};
  std::optional<RecognitionOrder> current_order_;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> active_session_;
  vinput::daemon::asr::BackendDescriptor active_backend_;
  bool current_is_command_ = false;
  std::string current_selected_text_;
  std::vector<int16_t> current_recording_pcm_;
  std::vector<int16_t> pending_chunk_pcm_;
  std::size_t current_sample_count_ = 0;
  float current_input_gain_ = 1.0f;
  std::string latest_final_text_;
  std::optional<std::chrono::steady_clock::time_point> recording_started_at_;
  std::optional<std::chrono::steady_clock::time_point> first_non_silent_at_;
  bool first_partial_logged_ = false;
  bool start_recording_in_progress_ = false;
  bool pending_asr_backend_reload_ = false;
  int notify_fd_ = -1;
  std::atomic<bool> pending_capture_stop_{false};
  std::atomic<bool> accepting_chunks_{false};
  std::atomic<bool> worker_running_{false};
  std::thread worker_;
};

} // namespace vinput::daemon::runtime
