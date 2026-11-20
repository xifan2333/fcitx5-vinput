#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "common/client/client_command.h"
#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"
#include "common/utils/unique_fd.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/output_ducker.h"
#include "daemon/runtime/audio_recording_buffer.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/lane_event_sink.h"
#include "daemon/runtime/recognition_pipeline.h"

namespace vinput::daemon::runtime {

enum class LanePhase : std::uint8_t {
  Idle,
  Starting,
  Recording,
  Stopping,
  Inferring,
  Postprocessing,
};

struct SessionLaneDependencies {
  using ConfigLoader = std::function<CoreConfig()>;
  using AsrSessionFactory = std::function<std::unique_ptr<vinput::daemon::asr::RecognitionSession>(
      const CoreConfig& settings, vinput::daemon::asr::BackendDescriptor* descriptor,
      std::string* error, const std::function<void()>& wake_callback)>;
  using PostprocessRunner = std::function<RecognitionPipelineResult(
      const PostprocessRequest& request, const CoreConfig& settings,
      const std::function<void()>& on_enter_postprocessing, const std::atomic<bool>* cancel_flag)>;

  vinput::daemon::audio::AudioInputSource* audio_source = nullptr;
  LaneEventSink* event_sink = nullptr;
  vinput::daemon::audio::OutputDucker* output_ducker = nullptr;
  ConfigLoader load_config;
  AsrSessionFactory create_asr_session;
  PostprocessRunner run_postprocess;
  int shared_notify_fd = -1;
  bool warn_on_clipped_head = true;
  std::chrono::milliseconds first_buffer_ready_timeout{1500};
};

// Single-threaded Actor managing one recording/recognition/postprocessing lane.
// All public control methods and ProcessPendingEvents() run on the owner event-loop
// thread; background threads (audio callbacks, ASR wakeups, worker thread) only post
// generation-tagged events into the thread-safe inbox and signal notify_fd_.
class SessionLane {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit SessionLane(SessionLaneDependencies deps);
  ~SessionLane();

  SessionLane(const SessionLane&) = delete;
  SessionLane& operator=(const SessionLane&) = delete;
  SessionLane(SessionLane&&) = delete;
  SessionLane& operator=(SessionLane&&) = delete;

  DbusService::MethodResult Start(bool ime, bool is_command, const std::string& selected_text = {},
                                  const std::string& capture_device = {},
                                  const std::string& default_scene_id = {});
  DbusService::MethodResult Stop(bool ime, const std::string& scene_id);
  DbusService::MethodResult Cancel(bool ime, bool commit_raw_text);
  DbusService::MethodResult InjectText(bool ime, vinput::client::InjectStage stage,
                                       const std::string& text, const std::string& scene_id = {});

  // Drains inbox events, incoming audio, readiness timeouts, and active ASR events.
  void ProcessPendingEvents();

  // Returns milliseconds until the next readiness deadline, or -1 if none is armed.
  [[nodiscard]] int NextTimeoutMs() const;

  [[nodiscard]] LanePhase CurrentPhase() const noexcept;
  [[nodiscard]] vinput::dbus::Status CurrentDbusStatus() const noexcept;
  [[nodiscard]] std::string GetStatus() const;
  [[nodiscard]] bool IsIdle() const noexcept;
  [[nodiscard]] int GetNotifyFd() const noexcept;

  // Notifies the lane that an FdStreamAudioSource reached EOF. Safe from any thread.
  void NotifySourceEof(std::uint64_t token);
  [[nodiscard]] std::uint64_t CurrentToken() const noexcept;

  void Shutdown();

private:
  enum class PostprocessingAction : std::uint8_t { None, CommitRaw, Discard };

  struct PendingStop {
    bool ime = true;
    std::string scene_id;
    TimePoint stop_at;
  };

  struct ActiveAttempt {
    std::uint64_t token = 0;
    bool ime = true;
    bool is_command = false;
    std::string selected_text;
    std::string default_scene_id;
    CoreConfig settings;
    bool ducked = false;
    bool first_buffer_received = false;
    bool first_buffer_timed_out = false;
    bool source_eof = false;
    bool asr_ready = false;
    bool first_chunk_inspected = false;
    bool clipped_head_warned = false;
    bool first_partial_logged = false;
    std::optional<PendingStop> pending_stop;
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> asr_session;
    vinput::daemon::asr::BackendDescriptor backend;
    std::string latest_final_text;
    PostprocessingAction postprocessing_action = PostprocessingAction::None;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    std::optional<TimePoint> start_received_at;
    std::optional<TimePoint> connect_returned_at;
    std::optional<TimePoint> first_buffer_at;
    std::optional<TimePoint> first_non_silent_at;
    std::optional<TimePoint> asr_ready_at;
    std::optional<TimePoint> recording_published_at;
    std::optional<TimePoint> stop_at;
  };

  struct FirstBufferEvent {
    std::uint64_t token = 0;
    int64_t timestamp_ns = 0;
  };
  struct AudioErrorEvent {
    std::uint64_t token = 0;
    std::string error;
  };
  struct SourceEofEvent {
    std::uint64_t token = 0;
  };
  struct AsrSessionReadyEvent {
    std::uint64_t token = 0;
    bool ok = false;
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
    vinput::daemon::asr::BackendDescriptor backend;
    std::string error;
  };
  struct RecognizedTextEvent {
    std::uint64_t token = 0;
    std::string text;
  };
  struct EnterPostprocessingEvent {
    std::uint64_t token = 0;
  };
  struct WorkerCompletedEvent {
    std::uint64_t token = 0;
    bool recognition_ok = true;
    std::string recognition_error;
    RecognitionPipelineResult pipeline_result;
  };
  struct WakeOnlyEvent {};

  using InboxEvent = std::variant<FirstBufferEvent, AudioErrorEvent, SourceEofEvent,
                                  AsrSessionReadyEvent, RecognizedTextEvent,
                                  EnterPostprocessingEvent, WorkerCompletedEvent, WakeOnlyEvent>;

  struct CreateAsrSessionTask {
    std::uint64_t token = 0;
    CoreConfig settings;
  };

  struct ProcessOrderTask {
    std::uint64_t token = 0;
    vinput::daemon::asr::AudioDeliveryMode audio_delivery_mode =
        vinput::daemon::asr::AudioDeliveryMode::Chunked;
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
    vinput::daemon::asr::BackendDescriptor backend;
    std::vector<int16_t> pcm;
    PostprocessRequest postprocess_request;
    CoreConfig settings;
    bool ime = true;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
  };

  using WorkerTask = std::variant<CreateAsrSessionTask, ProcessOrderTask>;

  void PostInboxEvent(InboxEvent event);
  void WakeMainThread() const noexcept;
  bool DrainInbox();
  std::optional<std::string> PendingTerminalAudioError() const;
  void HandleInboxEvent(InboxEvent& event);
  void HandleFirstBuffer(const FirstBufferEvent& ev);
  void HandleAudioError(const AudioErrorEvent& ev);
  void HandleSourceEof(const SourceEofEvent& ev);
  void HandleAsrSessionReady(AsrSessionReadyEvent& ev);
  void HandleRecognizedText(const RecognizedTextEvent& ev);
  void HandleEnterPostprocessing(const EnterPostprocessingEvent& ev);
  void HandleWorkerCompleted(WorkerCompletedEvent& ev);

  void DrainIncomingAudio();
  void HandleIncomingPcm(std::span<const int16_t> pcm);
  void InspectFirstChunkSpeech(std::span<const int16_t> pcm);
  void WarnClippedHeadIfNeeded();
  void FlushStreamingChunks();
  void PollActiveSessionEvents();
  bool ApplySingleRecognitionEvent(const vinput::daemon::asr::RecognitionEvent& event);
  void ApplyRecognitionEvents(const std::vector<vinput::daemon::asr::RecognitionEvent>& events);
  bool BeginAudioCapture(std::uint64_t token, const std::string& target, bool is_command,
                         std::string* error_out);
  std::size_t DrainTailOnStop();
  void CheckReadinessTimeout();
  void MaybeCompleteReadiness();
  void FinishCaptureAndSubmitOrDiscard();
  void LogHeadGapMetrics(std::size_t total_samples, std::size_t dropped_samples) const;
  void SubmitInferenceOrder(std::size_t total_samples);
  void TransitionToIdle(bool emit_status);
  void FailActiveAttempt(const std::string& error_message, bool abort_capture);
  void RestoreDuckerIfNeeded();
  void RetireSession(std::shared_ptr<vinput::daemon::asr::RecognitionSession> session);
  void FlushRetiredSessions();

  void EnsureWorkerStarted();
  void SubmitWorkerTask(WorkerTask task);
  void WorkerMain();
  void ExecuteCreateAsrSession(const CreateAsrSessionTask& task);
  void ExecuteProcessOrder(ProcessOrderTask task);
  bool RunRecognitionOnWorker(ProcessOrderTask& task, std::string* error_out);

  SessionLaneDependencies deps_;
  vinput::file::UniqueFd owned_notify_fd_;
  int notify_fd_ = -1;

  LanePhase phase_ = LanePhase::Idle;
  std::uint64_t next_token_ = 0;
  ActiveAttempt attempt_;
  AudioRecordingBuffer audio_buffer_;
  std::vector<std::shared_ptr<vinput::daemon::asr::RecognitionSession>> retired_sessions_;

  std::mutex inbox_mutex_;
  std::deque<InboxEvent> inbox_;

  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::optional<WorkerTask> pending_task_;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> in_flight_session_;
  std::shared_ptr<std::atomic<bool>> in_flight_cancel_flag_;
  bool worker_running_ = false;
  bool shutting_down_ = false;
  std::thread worker_;
};

} // namespace vinput::daemon::runtime
