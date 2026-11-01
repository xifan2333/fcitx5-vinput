#include "daemon/runtime/daemon_runtime_controller.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/config/core_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/utils/debug_log.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include "daemon/asr/runtime/async_recognition_session.h"
#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/audio_capture.h"
#include "daemon/audio/audio_types.h"
#include "daemon/audio/audio_utils.h"
#include "daemon/notify/desktop_notification_sender.h"
#include "daemon/remote/remote_text_service.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/recognition_pipeline.h"
#include "daemon/runtime/runtime_state_machine.h"
#include "daemon/runtime/session_lifecycle_manager.h"

namespace vinput::daemon::runtime {

namespace {

constexpr float kNonSilentPeakThreshold = 0.02F;
constexpr float kNonSilentRmsThreshold = 0.005F;

void LogRecognitionRequest(const vinput::daemon::asr::BackendDescriptor& descriptor,
                           std::size_t sample_count) {
  VLOG_DEBUG("ASR request provider={} type={} backend={} samples={}", descriptor.provider_id,
             descriptor.provider_type, descriptor.backend_id, sample_count);
}

bool UsesBufferedDelivery(const vinput::daemon::asr::BackendDescriptor& descriptor) {
  return descriptor.capabilities.audio_delivery_mode ==
         vinput::daemon::asr::AudioDeliveryMode::Buffered;
}

bool HasNonSilentAudio(std::span<const int16_t> pcm) {
  if (pcm.empty()) {
    return false;
  }

  double sum_squares = 0.0;
  float peak = 0.0F;
  for (const int16_t sample : pcm) {
    const float value = static_cast<float>(sample) / 32768.0F;
    const float abs_value = std::fabs(value);
    peak = std::max(peak, abs_value);
    sum_squares += static_cast<double>(value) * static_cast<double>(value);
  }

  const auto rms = static_cast<float>(std::sqrt(sum_squares / static_cast<double>(pcm.size())));
  return peak >= kNonSilentPeakThreshold || rms >= kNonSilentRmsThreshold;
}

struct RecognitionDispatchContext {
  std::string* latest_final_text = nullptr;
  std::string* latest_partial_text = nullptr;
  bool* first_partial_logged = nullptr;
  std::optional<std::chrono::steady_clock::time_point> recording_started_at;
  std::optional<std::chrono::steady_clock::time_point> first_non_silent_at;
  DbusService* dbus = nullptr;
};

void HandlePartialText(const vinput::daemon::asr::RecognitionEvent& event,
                       const RecognitionDispatchContext& ctx) {
  if (event.text.empty()) {
    return;
  }
  if (ctx.first_partial_logged != nullptr && !*ctx.first_partial_logged) {
    *ctx.first_partial_logged = true;
    const auto now = std::chrono::steady_clock::now();
    const long first_non_silent_ms = ctx.first_non_silent_at.has_value()
                                         ? static_cast<long>(vinput::time::MillisecondsSince(
                                               ctx.recording_started_at, *ctx.first_non_silent_at))
                                         : -1;
    VLOG_DEBUG("first partial after {} ms (first_non_silent_after={} ms)",
               vinput::time::MillisecondsSince(ctx.recording_started_at, now), first_non_silent_ms);
  }
  if (ctx.latest_partial_text != nullptr) {
    *ctx.latest_partial_text = event.text;
  }
  if (ctx.dbus != nullptr) {
    ctx.dbus->EmitRecognitionPartial(event.text);
  }
}

void HandleFinalText(const vinput::daemon::asr::RecognitionEvent& event,
                     const RecognitionDispatchContext& ctx) {
  if (event.text.empty()) {
    return;
  }
  if (ctx.latest_final_text != nullptr) {
    *ctx.latest_final_text = event.text;
  }
  if (ctx.latest_partial_text != nullptr) {
    *ctx.latest_partial_text = event.text;
  }
  if (ctx.dbus != nullptr) {
    ctx.dbus->EmitRecognitionPartial(event.text);
  }
}

void HandleError(const vinput::daemon::asr::RecognitionEvent& event,
                 const RecognitionDispatchContext& ctx) {
  if (event.error.empty() || ctx.dbus == nullptr) {
    return;
  }
  ctx.dbus->EmitDesktopNotification(
      vinput::daemon::notify::MakeDaemonErrorNotification(event.error));
}

void DispatchSingleEvent(const vinput::daemon::asr::RecognitionEvent& event,
                         const RecognitionDispatchContext& ctx) {
  switch (event.kind) {
  case vinput::daemon::asr::RecognitionEventKind::PartialText:
    HandlePartialText(event, ctx);
    break;
  case vinput::daemon::asr::RecognitionEventKind::FinalText:
    HandleFinalText(event, ctx);
    break;
  case vinput::daemon::asr::RecognitionEventKind::Error:
    HandleError(event, ctx);
    break;
  case vinput::daemon::asr::RecognitionEventKind::Completed:
    break;
  }
}

void ApplyRecognitionEvents(const std::vector<vinput::daemon::asr::RecognitionEvent>& events,
                            const RecognitionDispatchContext& ctx) {
  if (events.empty()) {
    return;
  }
  for (const auto& event : events) {
    DispatchSingleEvent(event, ctx);
  }
}

} // namespace

DaemonRuntimeController::DaemonRuntimeController(
    AudioCapture* capture, DbusService* dbus,
    vinput::daemon::asr::RecognitionSessionManager* recognition_manager,
    RecognitionPipeline* pipeline, vinput::daemon::remote::RemoteTextService* remote_text_service)
    : capture_(capture), dbus_(dbus), recognition_manager_(recognition_manager),
      pipeline_(pipeline), remote_text_service_(remote_text_service) {
  if (recognition_manager_ != nullptr) {
    recognition_manager_->SetReloadResultCallback([this](bool success, const std::string& message) {
      if (success) {
        return;
      }
      if (!dbus_ || message.empty()) {
        return;
      }
      dbus_->EmitDesktopNotification(vinput::daemon::notify::MakeDaemonErrorNotification(
          std::string(_("ASR backend reload failed. The previous backend may still be active.")) +
          "\n" + message));
    });
  }

  notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (notify_fd_ < 0) {
    VLOG_ERROR("failed to create runtime notify fd: {}", strerror(errno));
  } else if (capture_ != nullptr) {
    capture_->SetNotifyFd(notify_fd_);
  }
  session_lifecycle_.SetWakeCallback([this] { WakeMainThread(); });

  if (remote_text_service_ != nullptr) {
    remote_text_service_->SetSessionControlCallbacks(
        [this]() {
          const auto res = StartRecording();
          return res.ok;
        },
        [this](const std::string& scene_id) {
          const auto res = StopRecording(scene_id);
          return res.ok;
        });
  }
}

DaemonRuntimeController::~DaemonRuntimeController() {
  Shutdown();
  if (notify_fd_ >= 0) {
    close(notify_fd_);
    notify_fd_ = -1;
  }
}

DbusService::MethodResult DaemonRuntimeController::StartRecording() {
  return StartRecordingInternal(false, {});
}

DbusService::MethodResult
DaemonRuntimeController::StartCommandRecording(const std::string& selected_text) {
  return StartRecordingInternal(true, selected_text);
}

DbusService::MethodResult DaemonRuntimeController::CancelOperation(bool commit_raw_text) {
  bool apply_pending_reload = false;
  bool stop_capture = false;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;

  {
    const std::scoped_lock lock(state_mutex_);
    auto transition = state_machine_.OnCancel(commit_raw_text);
    if (!transition.accepted) {
      return DbusService::MethodResult::Failure(transition.rejection_reason);
    }

    if (transition.should_stop_capture) {
      accepting_chunks_.store(false, std::memory_order_relaxed);
      stop_capture = true;
      ResetRecordingStateLocked();
      session_to_cancel = session_lifecycle_.DetachActive();
      current_order_.reset();
      dbus_->EmitStatusChanged(vinput::dbus::StatusToString(transition.new_phase));
      VLOG_DEBUG("recording cancelled and discarded");
      apply_pending_reload = pending_asr_backend_reload_;
    } else if (transition.should_commit_raw_text || transition.should_discard_text) {
      postprocessing_cancel_requested_.store(true, std::memory_order_relaxed);
      VLOG_DEBUG("post-processing cancellation requested action={}",
                 commit_raw_text ? "commit-raw" : "discard");
    }
  }

  if (stop_capture) {
    capture_->StopAndGetBuffer();
    RestoreOutputIfDucked();
  }

  if (session_to_cancel) {
    session_lifecycle_.CancelAndRetireSession(std::move(session_to_cancel));
  }

  if (apply_pending_reload) {
    MaybeApplyPendingAsrBackendReload();
  }

  return DbusService::MethodResult::Success();
}

bool DaemonRuntimeController::SynchronizeAsrBackend(std::string* error) {
  auto runtime_settings = LoadCoreConfig();
  NormalizeCoreConfig(&runtime_settings);
  if (!recognition_manager_->SynchronizeBackend(runtime_settings, error)) {
    return false;
  }
  if (remote_text_service_ != nullptr &&
      !remote_text_service_->Synchronize(runtime_settings, error)) {
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

DbusService::MethodResult DaemonRuntimeController::ReloadAsrBackend() {
  {
    const std::scoped_lock lock(state_mutex_);
    if (state_machine_.CurrentPhase() != vinput::dbus::Status::Idle ||
        start_recording_in_progress_) {
      pending_asr_backend_reload_ = true;
      VLOG_DEBUG("ASR backend reload deferred until idle (phase: {} start_in_progress={})",
                 vinput::dbus::StatusToString(state_machine_.CurrentPhase()),
                 start_recording_in_progress_);
      return DbusService::MethodResult::Success();
    }
  }

  std::string error;
  if (!SynchronizeAsrBackend(&error)) {
    std::string message = _("ASR backend reload failed. The previous backend may still be active.");
    if (!error.empty()) {
      message += "\n" + error;
    }
    VLOG_WARN("{}", message);
    return DbusService::MethodResult::Failure(message);
  }

  return DbusService::MethodResult::Success();
}

void DaemonRuntimeController::MaybeApplyPendingAsrBackendReload() {
  {
    const std::scoped_lock lock(state_mutex_);
    if (!pending_asr_backend_reload_ ||
        state_machine_.CurrentPhase() != vinput::dbus::Status::Idle) {
      return;
    }
    pending_asr_backend_reload_ = false;
  }

  std::string error;
  if (!SynchronizeAsrBackend(&error)) {
    std::string message = _("ASR backend reload failed. The previous backend may still be active.");
    if (!error.empty()) {
      message += "\n" + error;
    }
    VLOG_WARN("{}", message);
    dbus_->EmitDesktopNotification(vinput::daemon::notify::MakeDaemonErrorNotification(message));
    return;
  }

  VLOG_DEBUG("deferred ASR backend reload applied while idle");
}

DbusService::MethodResult
DaemonRuntimeController::StartRecordingInternal(bool is_command, const std::string& selected_text) {
  const auto start_enter_at = std::chrono::steady_clock::now();
  // Drain any abort-path deferred stop before arming a new recording so a
  // rapid Tap-stop-Tap cannot destroy a just-reused stream.
  if (pending_capture_stop_.load(std::memory_order_acquire)) {
    FlushDeferredActions();
  }

  {
    const std::scoped_lock lock(state_mutex_);
    if (state_machine_.CurrentPhase() != vinput::dbus::Status::Idle ||
        start_recording_in_progress_) {
      VLOG_DEBUG("start rejected (phase: {} start_in_progress={})",
                 vinput::dbus::StatusToString(state_machine_.CurrentPhase()),
                 start_recording_in_progress_);
      return DbusService::MethodResult::Failure(
          _("Voice input daemon is busy. Please wait for the current request to finish."));
    }
    // Drop a stale deferred stop scheduled between the check above and now.
    pending_capture_stop_.store(false, std::memory_order_release);
    audio_buffer_.Reset();
    start_recording_in_progress_ = true;
    // Accept early mic callbacks into the startup buffer until CreateSession
    // completes. They must not be discarded before the backend is ready.
    accepting_chunks_.store(true, std::memory_order_relaxed);
  }

  auto runtime_settings = LoadCoreConfig();
  NormalizeCoreConfig(&runtime_settings);

  // Open the mic before CreateSession so ASR session setup cannot delay the
  // first capture buffers after a cold press. Chunks are only accepted after
  // both capture and session are armed below.
  std::string error;
  capture_->SetTargetObject(runtime_settings.global.captureDevice);
  capture_->SetNotifyFd(notify_fd_);
  AudioCapture::StartTiming capture_timing;
  if (!capture_->BeginRecording(&error, &capture_timing)) {
    std::string message =
        is_command ? _("Failed to start command recording.") : _("Failed to start recording.");
    if (!error.empty()) {
      message = std::string(vinput::str::StripSuffix(message, ".")) + ": " + error;
    }
    {
      const std::scoped_lock lock(state_mutex_);
      start_recording_in_progress_ = false;
      accepting_chunks_.store(false, std::memory_order_relaxed);
      audio_buffer_.Clear();
    }
    VLOG_WARN("{}", message);
    return DbusService::MethodResult::Failure(message);
  }

  vinput::daemon::asr::BackendDescriptor active_backend;
  const auto session_start_at = std::chrono::steady_clock::now();
  auto session = recognition_manager_->CreateSession(runtime_settings, &active_backend, &error);
  const long session_ms = static_cast<long>(vinput::time::MillisecondsSince(session_start_at));
  if (!session) {
    std::string message = _("Failed to start recognition session.");
    if (!error.empty()) {
      message += " " + error;
    }
    capture_->EndRecording();
    {
      const std::scoped_lock lock(state_mutex_);
      start_recording_in_progress_ = false;
      accepting_chunks_.store(false, std::memory_order_relaxed);
      audio_buffer_.Clear();
    }
    RestoreOutputIfDucked();
    ScheduleCaptureStopOnMainThread();
    VLOG_WARN("{}", message);
    return DbusService::MethodResult::Failure(message);
  }

  if (active_backend.capabilities.audio_delivery_mode ==
      vinput::daemon::asr::AudioDeliveryMode::Chunked) {
    // Isolate backend PushAudio/PollEvents on the session's worker thread;
    // the capture callback then only enqueues owned chunks. Installation
    // still happens under state_mutex_ below, so the AudioRecordingBuffer prefix
    // replay order is unchanged.
    auto async_session =
        std::make_unique<vinput::daemon::asr::AsyncRecognitionSession>(std::move(session));
    // Wake the main loop when the worker appends events or fails, so a
    // PartialText produced after the last audio chunk is not stranded until
    // the next chunk. The callback captures only the notify fd by value --
    // never `this`: it runs on the worker thread and may only write the
    // (non-blocking) eventfd. Ownership: every async session is held by
    // active_session_, current_order_, or deferred_cancelled_sessions_, and
    // Shutdown() cancels/destroys all of them (joining their workers in the
    // session destructor) before the destructor closes notify_fd_, so the
    // fd outlives every possible callback invocation.
    async_session->SetWakeCallback([notify_fd = notify_fd_] {
      if (notify_fd >= 0) {
        const uint64_t value = 1;
        (void)write(notify_fd, &value, sizeof(value));
      }
    });
    session = std::move(async_session);
  }

  bool abort_started_capture = false;
  {
    const std::scoped_lock lock(state_mutex_);
    if (!start_recording_in_progress_ ||
        state_machine_.CurrentPhase() != vinput::dbus::Status::Idle) {
      start_recording_in_progress_ = false;
      accepting_chunks_.store(false, std::memory_order_relaxed);
      audio_buffer_.Clear();
      abort_started_capture = true;
    } else {
      current_order_.reset();
      session_lifecycle_.InstallActiveSession(std::move(session));
      active_backend_ = active_backend;
      state_machine_.OnStartRecording(is_command, selected_text);
      audio_buffer_.SetGain(static_cast<float>(runtime_settings.asr.inputGain));
      audio_buffer_.Activate(UsesBufferedDelivery(active_backend_));
      latest_final_text_.clear();
      recording_started_at_ = std::chrono::steady_clock::now();
      first_non_silent_at_.reset();
      first_partial_logged_ = false;
      accepting_chunks_.store(true, std::memory_order_relaxed);
      start_recording_in_progress_ = false;
    }
  }

  if (abort_started_capture) {
    capture_->EndRecording();
    RestoreOutputIfDucked();
    ScheduleCaptureStopOnMainThread();
    return DbusService::MethodResult::Failure(
        _("Voice input daemon is busy. Please wait for the current request to finish."));
  }

  if (runtime_settings.global.duckOutputWhileRecording) {
    output_ducker_.Duck(runtime_settings.global.duckOutputVolume);
  }

  const long start_total_ms = static_cast<long>(vinput::time::MillisecondsSince(start_enter_at));
  VLOG_DEBUG("start timing command={} session_ms={} idle_gap_ms={} "
             "create_stream_ms={} set_active_ms={} stream_reused={} "
             "reuse_policy={} start_total_ms={} backend={}",
             is_command ? 1 : 0, session_ms, capture_timing.idle_gap_ms,
             capture_timing.create_stream_ms, capture_timing.set_active_ms,
             capture_timing.stream_reused ? 1 : 0, capture_timing.reuse_policy_enabled ? 1 : 0,
             start_total_ms, active_backend.backend_id);

  dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Recording));
  if (is_command) {
    VLOG_DEBUG("command recording started (selected_text length: {} chars)", selected_text.size());
  } else {
    VLOG_DEBUG("recording started");
  }
  return DbusService::MethodResult::Success();
}

void DaemonRuntimeController::DrainIncomingAudio() {
  if (capture_ == nullptr || !accepting_chunks_.load(std::memory_order_relaxed)) {
    return;
  }
  vinput::audio::AudioPcmChunk chunk;
  while (capture_->TryDequeue(chunk)) {
    if (chunk.size > 0) {
      HandleIncomingAudio(chunk.AsSpan());
    }
  }
}

void DaemonRuntimeController::HandleIncomingAudio(std::span<const int16_t> pcm) {
  if (!accepting_chunks_.load(std::memory_order_relaxed) || pcm.empty()) {
    return;
  }

  // AudioCapture starts before CreateSession. Until the session is ready,
  // collect every callback in order under the same state mutex used to
  // atomically install the live session.
  {
    const std::scoped_lock lock(state_mutex_);
    if (start_recording_in_progress_ && !session_lifecycle_.HasActiveSession()) {
      audio_buffer_.AppendStartup(pcm);
      return;
    }
  }

  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  std::optional<std::vector<int16_t>> chunk_to_push;
  bool streaming_mode = false;
  {
    const std::scoped_lock lock(state_mutex_);
    if (!session_lifecycle_.HasActiveSession()) {
      return;
    }
    session = session_lifecycle_.ActiveSession();

    if (!first_non_silent_at_.has_value()) {
      std::vector<int16_t> gained_pcm;
      const auto pcm_view =
          vinput::audio::PrepareGainAdjustedInput(pcm, audio_buffer_.Gain(), &gained_pcm);
      if (HasNonSilentAudio(pcm_view)) {
        first_non_silent_at_ = std::chrono::steady_clock::now();
        VLOG_DEBUG("first non-silent audio after {} ms",
                   vinput::time::MillisecondsSince(recording_started_at_, *first_non_silent_at_));
      }
    }

    audio_buffer_.Append(pcm);
    if (!UsesBufferedDelivery(active_backend_)) {
      streaming_mode = true;
      chunk_to_push = audio_buffer_.PopStreamingChunk();
    }
  }

  while (streaming_mode && chunk_to_push.has_value()) {
    std::string push_error;
    const bool pushed = session_lifecycle_.PushAudio(session, *chunk_to_push, &push_error);

    if (!pushed || !push_error.empty()) {
      FailActiveSession(session, push_error);
      return;
    }

    bool session_retired = false;
    {
      const std::scoped_lock lock(state_mutex_);
      if (!session_lifecycle_.IsActiveSession(session)) {
        session_retired = true;
      } else {
        chunk_to_push = audio_buffer_.PopStreamingChunk();
      }
    }

    if (session_retired) {
      session_lifecycle_.RetireSession(std::move(session));
      return;
    }

    if (!chunk_to_push.has_value()) {
      break;
    }
  }
}

void DaemonRuntimeController::RestoreOutputIfDucked() {
  output_ducker_.Restore();
}

void DaemonRuntimeController::ResetRecordingStateLocked() {
  audio_buffer_.Clear();
  latest_final_text_.clear();
  recording_started_at_.reset();
  first_non_silent_at_.reset();
  first_partial_logged_ = false;
  active_backend_ = {};
}

void DaemonRuntimeController::WakeMainThread() const {
  if (notify_fd_ >= 0) {
    uint64_t value = 1;
    (void)write(notify_fd_, &value, sizeof(value));
  }
}

bool DaemonRuntimeController::FailActiveSession(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
    const std::string& error_message) {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;
  {
    const std::scoped_lock lock(state_mutex_);
    if (!session_lifecycle_.IsActiveSession(session)) {
      return false;
    }
    VLOG_ERROR("failed to push audio chunk: {}", error_message);
    accepting_chunks_.store(false, std::memory_order_relaxed);
    ResetRecordingStateLocked();
    session_to_cancel = session_lifecycle_.DetachActive();
    state_machine_.OnAudioError();
  }

  if (session_to_cancel) {
    session_lifecycle_.CancelAndRetireSession(std::move(session_to_cancel));
  }

  dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Error));
  dbus_->EmitDesktopNotification(
      vinput::daemon::notify::MakeDaemonErrorNotification(error_message));
  capture_->EndRecording();
  RestoreOutputIfDucked();
  ScheduleCaptureStopOnMainThread();
  return true;
}

void DaemonRuntimeController::PollActiveSessionEvents() {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  std::optional<std::chrono::steady_clock::time_point> recording_started_at;
  std::optional<std::chrono::steady_clock::time_point> first_non_silent_at;
  SessionPollResult poll;
  {
    const std::scoped_lock lock(state_mutex_);
    if (state_machine_.CurrentPhase() != vinput::dbus::Status::Recording ||
        !session_lifecycle_.HasActiveSession() || UsesBufferedDelivery(active_backend_)) {
      return;
    }
    session = session_lifecycle_.ActiveSession();
    recording_started_at = recording_started_at_;
    first_non_silent_at = first_non_silent_at_;

    poll = session_lifecycle_.PollSessionEvents(session);
    if (!poll.events.empty()) {
      ApplyRecognitionEvents(poll.events, RecognitionDispatchContext{
                                              .latest_final_text = &latest_final_text_,
                                              .first_partial_logged = &first_partial_logged_,
                                              .recording_started_at = recording_started_at,
                                              .first_non_silent_at = first_non_silent_at,
                                              .dbus = dbus_,
                                          });
    }
  }

  if (poll.failure.has_value() && session) {
    FailActiveSession(session, *poll.failure);
  }
}

void DaemonRuntimeController::ScheduleCaptureStopOnMainThread() {
  pending_capture_stop_.store(true, std::memory_order_release);
  WakeMainThread();
}

DbusService::MethodResult DaemonRuntimeController::StopRecording(const std::string& scene_id) {
  bool apply_pending_reload = false;
  bool stop_capture = false;
  std::vector<int16_t> captured_pcm;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;
  {
    const std::scoped_lock lock(state_mutex_);
    if (state_machine_.CurrentPhase() != vinput::dbus::Status::Recording) {
      VLOG_DEBUG("stop rejected (phase: {})",
                 vinput::dbus::StatusToString(state_machine_.CurrentPhase()));
      return DbusService::MethodResult::Failure(_("Recording is not active."));
    }
    accepting_chunks_.store(false, std::memory_order_relaxed);
    stop_capture = true;
  }

  if (stop_capture) {
    // StopAndGetBuffer ends recording and deactivates/reuses the stream.
    // Avoid a prior EndRecording() so idle-grace is scheduled once.
    if (const auto first_buffer_ms = capture_->FirstBufferLatencyMs()) {
      VLOG_DEBUG("capture first_buffer_ms={} (at stop)", *first_buffer_ms);
    } else {
      VLOG_DEBUG("capture first_buffer_ms=-1 (no buffer before stop)");
    }
    const auto dropped_chunks = capture_->DroppedChunks();
    const auto dropped_samples = capture_->DroppedSamples();
    if (dropped_chunks > 0 || dropped_samples > 0) {
      VLOG_WARN("capture audio queue dropped {} chunks ({} samples) due to overflow",
                dropped_chunks, dropped_samples);
    }
    captured_pcm = capture_->StopAndGetBuffer();
    RestoreOutputIfDucked();
  }

  {
    const std::scoped_lock lock(state_mutex_);
    if (!session_lifecycle_.HasActiveSession()) {
      VLOG_DEBUG("recording stopped without active session");
      state_machine_.OnResetToIdle();
      current_order_.reset();
      dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Idle));
      VLOG_DEBUG("phase -> idle");
      apply_pending_reload = true;
    } else {
      if (!captured_pcm.empty()) {
        audio_buffer_.Append(captured_pcm);
      }

      const auto total_samples = audio_buffer_.TotalSampleCount();
      auto transition = state_machine_.OnStopRecording(
          total_samples, vinput::daemon::asr::kMinSamplesForRecognition);

      if (transition.is_too_short) {
        VLOG_DEBUG("recording too short, skipping inference: {} samples ({:.1f} ms)", total_samples,
                   static_cast<double>(total_samples) * 1000.0 / 16000.0);
        ResetRecordingStateLocked();
        session_to_cancel = session_lifecycle_.DetachActive();
        current_order_.reset();
        dbus_->EmitStatusChanged(vinput::dbus::StatusToString(transition.new_phase));
        VLOG_DEBUG("phase -> idle");
        apply_pending_reload = true;
      } else {
        current_order_ = RecognitionOrder{};
        current_order_->audio_delivery_mode = active_backend_.capabilities.audio_delivery_mode;
        current_order_->session = session_lifecycle_.DetachActive();
        current_order_->backend = active_backend_;
        if (UsesBufferedDelivery(active_backend_)) {
          current_order_->pcm = audio_buffer_.TakeBufferedPcm();
        } else {
          current_order_->pcm = audio_buffer_.DrainStreamingTail();
          if (!current_order_->pcm.empty()) {
            VLOG_DEBUG(
                "deferred final audio tail chunk to worker samples={} captured={} chunk_size={}",
                current_order_->pcm.size(), captured_pcm.size(),
                AudioRecordingBuffer::kStreamingChunkSamples);
          }
        }
        current_order_->recognized_text = latest_final_text_;
        current_order_->scene_id = scene_id;
        current_order_->is_command = state_machine_.IsCommand();
        current_order_->selected_text = state_machine_.SelectedText();
        LogRecognitionRequest(active_backend_, total_samples);
        active_backend_ = {};
        dbus_->EmitStatusChanged(vinput::dbus::StatusToString(transition.new_phase));
        worker_cv_.notify_one();
        VLOG_DEBUG("recording stopped");
      }
    }
  }

  if (session_to_cancel) {
    session_lifecycle_.CancelAndRetireSession(std::move(session_to_cancel));
  }

  if (apply_pending_reload) {
    MaybeApplyPendingAsrBackendReload();
  }
  return DbusService::MethodResult::Success();
}

std::string DaemonRuntimeController::GetStatus() const {
  const std::scoped_lock lock(state_mutex_);
  return vinput::dbus::StatusToString(state_machine_.CurrentPhase());
}

vinput::dbus::AsrBackendState DaemonRuntimeController::GetAsrBackendState() const {
  const auto snapshot = recognition_manager_->GetReloadSnapshot();
  vinput::dbus::AsrBackendState state{
      .target_provider_id = snapshot.target_provider_id,
      .target_model_id = snapshot.target_model_id,
      .effective_provider_id = snapshot.effective_provider_id,
      .effective_model_id = snapshot.effective_model_id,
      .last_error = snapshot.last_error,
      .reload_in_progress = snapshot.reload_in_progress,
      .has_effective_backend = snapshot.has_effective_backend,
      .remote_endpoints = {},
  };
  if (remote_text_service_ != nullptr) {
    state.remote_endpoints = remote_text_service_->ListEndpoints();
  }
  return state;
}

int DaemonRuntimeController::GetNotifyFd() const {
  return notify_fd_;
}

void DaemonRuntimeController::FlushDeferredActions() {
  // Drain the notify fd before swapping the deferred vector: a session
  // enqueued after the swap must keep its wake for the next main-loop
  // iteration, otherwise it could linger in the vector until an unrelated
  // notification. A session enqueued before the swap is taken below.
  if (notify_fd_ >= 0) {
    uint64_t value = 0;
    while (read(notify_fd_, &value, sizeof(value)) == sizeof(value)) {
    }
  }

  session_lifecycle_.FlushReapedSessions();

  DrainIncomingAudio();

  // Worker wake-ups write notify_fd_ without setting pending_capture_stop_,
  // so poll the active session before the early return below. This only
  // ever touches active_session_ (still recording); a session handed to
  // WorkerMain by StopRecording is consumed exclusively there.
  PollActiveSessionEvents();
  if (!pending_capture_stop_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  // Abort-path cleanup only. If a newer StartRecording already began, do not
  // tear down its capture (rapid Tap-stop-Tap / failed-start races).
  bool skip_teardown = false;
  vinput::dbus::Status phase_snapshot = vinput::dbus::Status::Idle;
  bool start_in_progress = false;
  {
    const std::scoped_lock lock(state_mutex_);
    phase_snapshot = state_machine_.CurrentPhase();
    start_in_progress = start_recording_in_progress_;
    skip_teardown = start_recording_in_progress_ || capture_->IsRecording() ||
                    phase_snapshot == vinput::dbus::Status::Recording ||
                    phase_snapshot == vinput::dbus::Status::Inferring ||
                    phase_snapshot == vinput::dbus::Status::Postprocessing;
  }
  if (skip_teardown) {
    VLOG_DEBUG("deferred capture stop skipped (start_in_progress={} recording={} phase={})",
               start_in_progress ? 1 : 0, capture_->IsRecording() ? 1 : 0,
               vinput::dbus::StatusToString(phase_snapshot));
    RestoreOutputIfDucked();
    return;
  }

  if (AudioCapture::StreamReuseEnabled()) {
    // EndRecording/StopAndGetBuffer already deactivated the stream; keep it
    // for warm set_active until idle grace expires.
    VLOG_DEBUG("deferred capture stop: keeping inactive reusable stream");
  } else {
    capture_->Stop();
  }
  RestoreOutputIfDucked();
  ResetToIdle();
}

void DaemonRuntimeController::StartWorker() {
  if (worker_running_) {
    return;
  }
  worker_running_ = true;
  worker_ = std::thread([this]() { WorkerMain(); });
}

void DaemonRuntimeController::Shutdown() {
  if (remote_text_service_ != nullptr) {
    remote_text_service_->SetSessionControlCallbacks(nullptr, nullptr);
  }
  accepting_chunks_.store(false, std::memory_order_relaxed);
  pending_capture_stop_.store(false, std::memory_order_relaxed);
  if (capture_ != nullptr) {
    capture_->SetNotifyFd(-1);
    capture_->Stop();
  }
  RestoreOutputIfDucked();
  {
    const std::scoped_lock lock(state_mutex_);
    start_recording_in_progress_ = false;
    worker_running_ = false;
    state_machine_.Clear();
  }
  session_lifecycle_.CancelAndRetireActive();
  worker_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  session_lifecycle_.FlushReapedSessions();
  session_lifecycle_.Clear();
}

void DaemonRuntimeController::EnterPostprocessing(bool /*command_mode*/) {
  vinput::dbus::Status next_phase = vinput::dbus::Status::Postprocessing;
  {
    const std::scoped_lock lock(state_mutex_);
    const auto res = state_machine_.OnInferenceComplete(true);
    next_phase = res.new_phase;
  }
  dbus_->EmitStatusChanged(vinput::dbus::StatusToString(next_phase));
}

void DaemonRuntimeController::ResetToIdle() {
  {
    const std::scoped_lock lock(state_mutex_);
    start_recording_in_progress_ = false;
    postprocessing_cancel_requested_.store(false, std::memory_order_relaxed);
    state_machine_.OnResetToIdle();
    current_order_.reset();
    ResetRecordingStateLocked();
  }
  RestoreOutputIfDucked();
  dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Idle));
  VLOG_DEBUG("phase -> idle");
  MaybeApplyPendingAsrBackendReload();
}

void DaemonRuntimeController::WorkerMain() {
  while (worker_running_) {
    RecognitionOrder order;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      worker_cv_.wait(lock,
                      [&]() { return current_order_.has_value() || !worker_running_.load(); });
      if (!worker_running_ && !current_order_.has_value()) {
        break;
      }
      if (!current_order_.has_value()) {
        continue;
      }
      order = std::move(*current_order_);
    }

    try {
      bool recognition_failed = false;
      auto runtime_settings = LoadCoreConfig();
      NormalizeCoreConfig(&runtime_settings);

      if (order.session) {
        if (runtime_settings.asr.normalizeAudio && !order.pcm.empty() &&
            order.audio_delivery_mode == vinput::daemon::asr::AudioDeliveryMode::Buffered) {
          // Apply peak normalization at the device boundary before inference.
          std::vector<float> float_samples(order.pcm.size());
          for (std::size_t i = 0; i < order.pcm.size(); ++i) {
            float_samples[i] = static_cast<float>(order.pcm[i]) / 32768.0F;
          }
          order.pcm.clear();
          order.pcm.shrink_to_fit();
          vinput::audio::PeakNormalize(float_samples);

          std::string push_error;
          if (!session_lifecycle_.PushAudio(order.session, float_samples, &push_error)) {
            throw std::runtime_error(push_error);
          }
        } else if (!order.pcm.empty()) {
          std::string push_error;
          if (!session_lifecycle_.PushAudio(order.session, order.pcm, &push_error)) {
            throw std::runtime_error(push_error);
          }
          order.pcm.clear();
        }

        std::string recognition_error;
        auto result =
            session_lifecycle_.FinishSessionAndCollectResult(order.session, &recognition_error);
        if (!result.ok || !result.error.empty()) {
          // Preserve the structured error classification (as the previous
          // error path did) instead of routing this through the generic
          // exception handler, which would downgrade it to a raw error.
          const std::string failure =
              result.error.empty() ? "Failed to finish recognition session." : result.error;
          VLOG_ERROR("recognition error: {}", failure);
          dbus_->EmitDesktopNotification(
              vinput::daemon::notify::MakeDaemonErrorNotification(failure));
          recognition_failed = true;
        } else if (!result.text.empty()) {
          order.recognized_text = std::move(result.text);
          // RecognitionPartial is also the protocol's transcript-update signal.
          dbus_->EmitRecognitionPartial(order.recognized_text);
        }
        order.pcm.clear();
        order.session.reset();
      }

      if (recognition_failed) {
        ResetToIdle();
        continue;
      }

      postprocessing_cancel_requested_.store(false, std::memory_order_relaxed);
      auto pipeline_result = pipeline_->Process(
          order, runtime_settings, [&]() { EnterPostprocessing(order.is_command); },
          &postprocessing_cancel_requested_);
      PostprocessingState postprocessing_action = PostprocessingState::Inactive;
      {
        const std::scoped_lock lock(state_mutex_);
        postprocessing_action = state_machine_.TakePostprocessingState();
      }
      if (postprocessing_action == PostprocessingState::CommitRaw) {
        pipeline_result.payload.commitText = order.recognized_text;
        pipeline_result.payload.candidates = {
            {.text = order.recognized_text, .source = vinput::result::kSourceRaw}};
        pipeline_result.errors.clear();
      } else if (postprocessing_action == PostprocessingState::Discard) {
        pipeline_result.payload = {};
        pipeline_result.errors.clear();
      }
      for (const auto& error : pipeline_result.errors) {
        if (!error.empty()) {
          VLOG_ERROR("processing error: {}", error);
        }
        dbus_->EmitDesktopNotification(vinput::daemon::notify::MakeDaemonErrorNotification(error));
      }
      dbus_->EmitRecognitionResult(vinput::result::Serialize(pipeline_result.payload));
    } catch (const std::exception& e) {
      {
        const std::scoped_lock lock(state_mutex_);
        (void)state_machine_.TakePostprocessingState();
      }
      VLOG_ERROR("worker exception: {}", e.what());
      dbus_->EmitDesktopNotification(vinput::daemon::notify::MakeDaemonErrorNotification(e.what()));
    } catch (...) {
      {
        const std::scoped_lock lock(state_mutex_);
        (void)state_machine_.TakePostprocessingState();
      }
      VLOG_ERROR("worker unknown exception");
      dbus_->EmitDesktopNotification(vinput::daemon::notify::MakeDaemonErrorNotification(
          _("Unknown error during processing.")));
    }

    ResetToIdle();
  }
}

} // namespace vinput::daemon::runtime
