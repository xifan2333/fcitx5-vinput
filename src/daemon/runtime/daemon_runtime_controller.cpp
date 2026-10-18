#include "daemon/runtime/daemon_runtime_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include "common/dbus/error_info.h"
#include "common/i18n.h"
#include "common/utils/debug_log.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include "daemon/asr/runtime/async_recognition_session.h"
#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/audio/audio_utils.h"
#include "daemon/remote/remote_text_service.h"
#include "daemon/runtime/dbus_service.h"

namespace vinput::daemon::runtime {

namespace {

constexpr std::size_t kStreamingChunkSamples = 800;
constexpr const char* kFailedToPushAudioError = "Failed to push audio chunk.";
constexpr const char* kFailedToFinishSessionError = "Failed to finish recognition session.";
constexpr float kNonSilentPeakThreshold = 0.02f;
constexpr float kNonSilentRmsThreshold = 0.005f;

void LogRecognitionRequest(const vinput::daemon::asr::BackendDescriptor& descriptor,
                           std::size_t sample_count) {
  vinput::debug::Log("ASR request provider=%s type=%s backend=%s samples=%zu\n",
                     descriptor.provider_id.c_str(), descriptor.provider_type.c_str(),
                     descriptor.backend_id.c_str(), sample_count);
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
  float peak = 0.0f;
  for (int16_t sample : pcm) {
    const float value = static_cast<float>(sample) / 32768.0f;
    const float abs_value = std::fabs(value);
    peak = std::max(peak, abs_value);
    sum_squares += static_cast<double>(value) * static_cast<double>(value);
  }

  const float rms = static_cast<float>(std::sqrt(sum_squares / pcm.size()));
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
    vinput::debug::Log(
        "first partial after %ld ms (first_non_silent_after=%ld ms)\n",
        static_cast<long>(vinput::time::MillisecondsSince(ctx.recording_started_at, now)),
        first_non_silent_ms);
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
  ctx.dbus->EmitNotification(vinput::dbus::ClassifyErrorText(event.error));
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

vinput::daemon::asr::RecognitionRunResult FinishSessionAndCollectResult(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
    std::mutex* session_io_mutex, std::string* error) {
  vinput::daemon::asr::RecognitionRunResult result;
  if (!session) {
    if (error) {
      *error = "Recognition session is not initialized.";
    }
    result.available = false;
    result.ok = false;
    result.error = error ? *error : "Recognition session is not initialized.";
    return result;
  }

  result.available = true;
  std::string session_error;
  std::vector<vinput::daemon::asr::RecognitionEvent> events;
  {
    std::lock_guard<std::mutex> session_lock(*session_io_mutex);
    if (!session->Finish(&session_error)) {
      result.ok = false;
      result.error = session_error.empty() ? kFailedToFinishSessionError : session_error;
    }
    events = session->PollEvents();
  }

  for (auto& event : events) {
    switch (event.kind) {
    case vinput::daemon::asr::RecognitionEventKind::PartialText:
      break;
    case vinput::daemon::asr::RecognitionEventKind::FinalText:
      result.text = std::move(event.text);
      break;
    case vinput::daemon::asr::RecognitionEventKind::Error:
      result.ok = false;
      if (!event.error.empty()) {
        result.error = std::move(event.error);
      }
      break;
    case vinput::daemon::asr::RecognitionEventKind::Completed:
      break;
    }
  }

  if (error) {
    *error = result.error;
  }
  return result;
}

} // namespace

DaemonRuntimeController::DaemonRuntimeController(
    AudioCapture* capture, DbusService* dbus,
    vinput::daemon::asr::RecognitionSessionManager* recognition_manager,
    RecognitionPipeline* pipeline, vinput::daemon::remote::RemoteTextService* remote_text_service)
    : capture_(capture), dbus_(dbus), recognition_manager_(recognition_manager),
      pipeline_(pipeline), remote_text_service_(remote_text_service) {
  if (recognition_manager_) {
    recognition_manager_->SetReloadResultCallback([this](bool success, const std::string& message) {
      if (success) {
        return;
      }
      if (!dbus_ || message.empty()) {
        return;
      }
      dbus_->EmitNotification(
          vinput::dbus::MakeRawError("Failed to apply ASR backend reload. " + message));
    });
  }

  notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (notify_fd_ < 0) {
    fprintf(stderr, "vinput-daemon: failed to create runtime notify fd: %s\n", strerror(errno));
  }

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
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (phase_ == vinput::dbus::Status::Recording) {
      accepting_chunks_.store(false, std::memory_order_relaxed);
      stop_capture = true;
      session_to_cancel = ReleaseActiveSessionLocked();
      phase_ = vinput::dbus::Status::Idle;
      current_order_.reset();
      current_recording_pcm_.clear();
      pending_chunk_pcm_.clear();
      current_sample_count_ = 0;
      dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Idle));
      vinput::debug::Log("recording cancelled and discarded\n");
      apply_pending_reload = pending_asr_backend_reload_;
    } else if (phase_ == vinput::dbus::Status::Postprocessing) {
      const bool accepted =
          postprocessing_state_ == PostprocessingState::Dictation ||
          (!commit_raw_text && postprocessing_state_ == PostprocessingState::Command);
      if (accepted) {
        postprocessing_state_ =
            commit_raw_text ? PostprocessingState::CommitRaw : PostprocessingState::Discard;
        postprocessing_cancel_requested_.store(true, std::memory_order_relaxed);
        vinput::debug::Log("post-processing cancellation requested action=%s\n",
                           commit_raw_text ? "commit-raw" : "discard");
      } else {
        return DbusService::MethodResult::Failure(
            _("Requested post-processing action is not available."));
      }
    } else if (phase_ == vinput::dbus::Status::Idle) {
      return DbusService::MethodResult::Success();
    } else {
      return DbusService::MethodResult::Failure(_("No cancellable operation in current phase."));
    }
  }

  if (stop_capture) {
    capture_->StopAndGetBuffer();
    RestoreOutputIfDucked();
  }

  if (session_to_cancel) {
    {
      const std::scoped_lock io_lock(session_io_mutex_);
      session_to_cancel->Cancel();
    }
    // Do not let session_to_cancel destruct here: destroying an async
    // session joins its worker on this D-Bus thread. Retire it on the main
    // thread instead. Cancel() is idempotent, so exactly-once is preserved.
    RetireCancelledSession(std::move(session_to_cancel));
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
  if (remote_text_service_ && !remote_text_service_->Synchronize(runtime_settings, error)) {
    return false;
  }
  if (error) {
    error->clear();
  }
  return true;
}

DbusService::MethodResult DaemonRuntimeController::ReloadAsrBackend() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (phase_ != vinput::dbus::Status::Idle || start_recording_in_progress_) {
      pending_asr_backend_reload_ = true;
      vinput::debug::Log(
          "ASR backend reload deferred until idle (phase: %s start_in_progress=%d)\n",
          vinput::dbus::StatusToString(phase_), start_recording_in_progress_);
      return DbusService::MethodResult::Success();
    }
  }

  std::string error;
  if (!SynchronizeAsrBackend(&error)) {
    std::string message = "Failed to reload ASR backend.";
    if (!error.empty()) {
      message += " " + error;
    }
    fprintf(stderr, "vinput-daemon: %s\n", message.c_str());
    return DbusService::MethodResult::Failure(message);
  }

  return DbusService::MethodResult::Success();
}

void DaemonRuntimeController::MaybeApplyPendingAsrBackendReload() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!pending_asr_backend_reload_ || phase_ != vinput::dbus::Status::Idle) {
      return;
    }
    pending_asr_backend_reload_ = false;
  }

  std::string error;
  if (!SynchronizeAsrBackend(&error)) {
    std::string message = "Failed to apply deferred ASR backend reload.";
    if (!error.empty()) {
      message += " " + error;
    }
    fprintf(stderr, "vinput-daemon: %s\n", message.c_str());
    dbus_->EmitNotification(vinput::dbus::MakeRawError(message));
    return;
  }

  vinput::debug::Log("deferred ASR backend reload applied while idle\n");
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
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (phase_ != vinput::dbus::Status::Idle || start_recording_in_progress_) {
      vinput::debug::Log("start rejected (phase: %s start_in_progress=%d)\n",
                         vinput::dbus::StatusToString(phase_), start_recording_in_progress_);
      return DbusService::MethodResult::Failure("Daemon is busy.");
    }
    // Drop a stale deferred stop scheduled between the check above and now.
    pending_capture_stop_.store(false, std::memory_order_release);
    startup_pcm_buffer_.Clear();
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
  capture_->SetChunkCallback([this](std::span<const int16_t> pcm) { HandleIncomingAudio(pcm); });
  AudioCapture::StartTiming capture_timing;
  if (!capture_->BeginRecording(&error, &capture_timing)) {
    std::string message =
        is_command ? "Failed to start command recording." : "Failed to start recording.";
    if (!error.empty()) {
      message = std::string(vinput::str::StripSuffix(message, ".")) + ": " + error;
    }
    capture_->SetChunkCallback({});
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      start_recording_in_progress_ = false;
      accepting_chunks_.store(false, std::memory_order_relaxed);
      startup_pcm_buffer_.Clear();
    }
    fprintf(stderr, "vinput-daemon: %s\n", message.c_str());
    return DbusService::MethodResult::Failure(message);
  }

  vinput::daemon::asr::BackendDescriptor active_backend;
  const auto session_start_at = std::chrono::steady_clock::now();
  auto session = recognition_manager_->CreateSession(runtime_settings, &active_backend, &error);
  const long session_ms = static_cast<long>(vinput::time::MillisecondsSince(session_start_at));
  if (!session) {
    std::string message = "Failed to start recognition session.";
    if (!error.empty()) {
      message += " " + error;
    }
    capture_->EndRecording();
    capture_->SetChunkCallback({});
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      start_recording_in_progress_ = false;
      accepting_chunks_.store(false, std::memory_order_relaxed);
      startup_pcm_buffer_.Clear();
    }
    RestoreOutputIfDucked();
    ScheduleCaptureStopOnMainThread();
    fprintf(stderr, "vinput-daemon: %s\n", message.c_str());
    return DbusService::MethodResult::Failure(message);
  }

  if (active_backend.capabilities.audio_delivery_mode ==
      vinput::daemon::asr::AudioDeliveryMode::Chunked) {
    // Isolate backend PushAudio/PollEvents on the session's worker thread;
    // the capture callback then only enqueues owned chunks. Installation
    // still happens under state_mutex_ below, so the StartupPcmBuffer prefix
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
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!start_recording_in_progress_ || phase_ != vinput::dbus::Status::Idle) {
      start_recording_in_progress_ = false;
      accepting_chunks_.store(false, std::memory_order_relaxed);
      startup_pcm_buffer_.Clear();
      abort_started_capture = true;
    } else {
      current_order_.reset();
      active_session_ =
          std::shared_ptr<vinput::daemon::asr::RecognitionSession>(std::move(session));
      active_backend_ = active_backend;
      current_is_command_ = is_command;
      current_selected_text_ = selected_text;
      current_recording_pcm_.clear();
      pending_chunk_pcm_.clear();
      current_sample_count_ = 0;
      current_input_gain_ = static_cast<float>(runtime_settings.asr.inputGain);
      if (UsesBufferedDelivery(active_backend_)) {
        // Buffered recognition uses the complete AudioCapture buffer on Stop.
        startup_pcm_buffer_.Clear();
      } else {
        // Replay the initialization prefix first, before live callbacks can
        // acquire state_mutex_ and append their later chunks.
        pending_chunk_pcm_ = startup_pcm_buffer_.TakeWithGain(current_input_gain_);
      }
      latest_final_text_.clear();
      recording_started_at_ = std::chrono::steady_clock::now();
      first_non_silent_at_.reset();
      first_partial_logged_ = false;
      accepting_chunks_.store(true, std::memory_order_relaxed);
      phase_ = vinput::dbus::Status::Recording;
      start_recording_in_progress_ = false;
    }
  }

  if (abort_started_capture) {
    capture_->EndRecording();
    capture_->SetChunkCallback({});
    RestoreOutputIfDucked();
    ScheduleCaptureStopOnMainThread();
    return DbusService::MethodResult::Failure("Daemon is busy.");
  }

  if (runtime_settings.global.duckOutputWhileRecording) {
    output_ducker_.Duck(runtime_settings.global.duckOutputVolume);
  }

  const long start_total_ms = static_cast<long>(vinput::time::MillisecondsSince(start_enter_at));
  vinput::debug::Log("start timing command=%d session_ms=%ld idle_gap_ms=%ld "
                     "create_stream_ms=%ld set_active_ms=%ld stream_reused=%d "
                     "reuse_policy=%d start_total_ms=%ld backend=%s\n",
                     is_command ? 1 : 0, session_ms, capture_timing.idle_gap_ms,
                     capture_timing.create_stream_ms, capture_timing.set_active_ms,
                     capture_timing.stream_reused ? 1 : 0,
                     capture_timing.reuse_policy_enabled ? 1 : 0, start_total_ms,
                     active_backend.backend_id.c_str());

  dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Recording));
  if (is_command) {
    vinput::debug::Log("command recording started (selected_text length: %zu chars)\n",
                       selected_text.size());
  } else {
    vinput::debug::Log("recording started\n");
  }
  return DbusService::MethodResult::Success();
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
    if (start_recording_in_progress_ && !active_session_) {
      startup_pcm_buffer_.Append(pcm);
      return;
    }
  }

  // Apply gain at the device boundary so all backends see processed audio.
  // At the default unity gain this borrows `pcm` instead of copying it.
  std::vector<int16_t> gained_pcm;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  std::vector<int16_t> chunk_to_push;
  std::optional<std::chrono::steady_clock::time_point> recording_started_at;
  std::optional<std::chrono::steady_clock::time_point> first_non_silent_at;
  bool streaming_mode = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!active_session_) {
      return;
    }
    session = active_session_;
    const auto pcm_view =
        vinput::audio::PrepareGainAdjustedInput(pcm, current_input_gain_, &gained_pcm);

    if (!first_non_silent_at_.has_value() && HasNonSilentAudio(pcm_view)) {
      first_non_silent_at_ = std::chrono::steady_clock::now();
      vinput::debug::Log("first non-silent audio after %ld ms\n",
                         static_cast<long>(vinput::time::MillisecondsSince(recording_started_at_,
                                                                           *first_non_silent_at_)));
    }

    if (UsesBufferedDelivery(active_backend_)) {
      // AudioCapture owns the full PCM until StopAndGetBuffer() overwrites
      // current_recording_pcm_; only the sample count is needed here.
      current_sample_count_ += pcm_view.size();
    } else {
      streaming_mode = true;
      recording_started_at = recording_started_at_;
      first_non_silent_at = first_non_silent_at_;
      pending_chunk_pcm_.insert(pending_chunk_pcm_.end(), pcm_view.begin(), pcm_view.end());
      if (pending_chunk_pcm_.size() >= kStreamingChunkSamples) {
        chunk_to_push.assign(pending_chunk_pcm_.begin(),
                             pending_chunk_pcm_.begin() +
                                 static_cast<std::ptrdiff_t>(kStreamingChunkSamples));
      }
    }
  }

  while (streaming_mode && !chunk_to_push.empty()) {
    std::string push_error;
    bool pushed = false;
    {
      std::lock_guard<std::mutex> session_lock(session_io_mutex_);
      pushed = session->PushAudio(chunk_to_push, &push_error);
    }
    if (!pushed && push_error.empty()) {
      push_error = kFailedToPushAudioError;
    }

    if (!pushed || !push_error.empty()) {
      FailActiveSession(session, push_error);
      return;
    }

    bool keep_processing = false;
    bool session_retired = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (active_session_ != session) {
        // This local reference may be the session's last owner; releasing
        // it here would destroy the async session (Cancel + worker join)
        // on the capture callback. Transfer it to the deferred main-thread
        // retirement vector while still under state_mutex_, then wake the
        // main thread only after the transfer (see RetireCancelledSession).
        // No Cancel is issued here: whoever replaced active_session_
        // already owns cancellation of the old session, and the deferred
        // vector only holds an extra reference if another owner (e.g.
        // current_order_) still has one, so a newer session is unaffected.
        deferred_cancelled_sessions_.push_back(session);
        session_retired = true;
      } else {
        // Drain and apply atomically under state_mutex_ (PollActiveSessionEvents
        // does the same on the main thread): draining here and applying later
        // could let a newer batch from the main thread be applied first and
        // then be overwritten by this older batch.
        std::vector<vinput::daemon::asr::RecognitionEvent> events;
        if (pushed) {
          const std::scoped_lock session_lock(session_io_mutex_);
          events = session->PollEvents();
        }

        current_sample_count_ += chunk_to_push.size();
        pending_chunk_pcm_.erase(pending_chunk_pcm_.begin(),
                                 pending_chunk_pcm_.begin() +
                                     static_cast<std::ptrdiff_t>(chunk_to_push.size()));
        if (!events.empty()) {
          ApplyRecognitionEvents(events, RecognitionDispatchContext{
                                             .latest_final_text = &latest_final_text_,
                                             .first_partial_logged = &first_partial_logged_,
                                             .recording_started_at = recording_started_at_,
                                             .first_non_silent_at = first_non_silent_at_,
                                             .dbus = dbus_,
                                         });
        }
        keep_processing = pending_chunk_pcm_.size() >= kStreamingChunkSamples;
        if (keep_processing) {
          chunk_to_push.assign(pending_chunk_pcm_.begin(),
                               pending_chunk_pcm_.begin() +
                                   static_cast<std::ptrdiff_t>(kStreamingChunkSamples));
          recording_started_at = recording_started_at_;
          first_non_silent_at = first_non_silent_at_;
        } else {
          chunk_to_push.clear();
        }
      }
    }

    if (session_retired) {
      WakeMainThread();
      return;
    }

    if (!keep_processing) {
      break;
    }
  }
}

void DaemonRuntimeController::RestoreOutputIfDucked() {
  output_ducker_.Restore();
}

void DaemonRuntimeController::WakeMainThread() const {
  if (notify_fd_ >= 0) {
    uint64_t value = 1;
    (void)write(notify_fd_, &value, sizeof(value));
  }
}

void DaemonRuntimeController::RetireCancelledSession(
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> session) {
  if (!session) {
    return;
  }
  {
    const std::scoped_lock lock(state_mutex_);
    deferred_cancelled_sessions_.push_back(std::move(session));
  }
  // Wake only after the deferred vector owns the session: an earlier wake
  // could let FlushDeferredActions clear it before the transfer, making the
  // caller's thread the last owner again.
  WakeMainThread();
}

bool DaemonRuntimeController::FailActiveSession(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
    const std::string& error_message) {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;
  {
    const std::scoped_lock lock(state_mutex_);
    if (active_session_ != session) {
      return false;
    }
    fprintf(stderr, "vinput-daemon: failed to push audio chunk: %s\n", error_message.c_str());
    accepting_chunks_.store(false, std::memory_order_relaxed);
    session_to_cancel = ReleaseActiveSessionLocked();
    phase_ = vinput::dbus::Status::Error;
  }
  if (session_to_cancel) {
    {
      const std::scoped_lock session_lock(session_io_mutex_);
      session_to_cancel->Cancel();
    }
    // Destroying the async session joins its worker; do that on the main
    // thread (FlushDeferredActions), not on this capture callback.
    const std::scoped_lock lock(state_mutex_);
    deferred_cancelled_sessions_.push_back(std::move(session_to_cancel));
  }
  // phase_ is already Error and the session is no longer active, so a
  // repeated wake-up for the same failure is discarded by the identity
  // checks in PollActiveSessionEvents/FailActiveSession: notified once.
  dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Error));
  dbus_->EmitNotification(vinput::dbus::MakeRawError(error_message));
  capture_->EndRecording();
  RestoreOutputIfDucked();
  ScheduleCaptureStopOnMainThread();
  return true;
}

void DaemonRuntimeController::PollActiveSessionEvents() {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  std::optional<std::chrono::steady_clock::time_point> recording_started_at;
  std::optional<std::chrono::steady_clock::time_point> first_non_silent_at;
  {
    const std::scoped_lock lock(state_mutex_);
    if (phase_ != vinput::dbus::Status::Recording || !active_session_ ||
        UsesBufferedDelivery(active_backend_)) {
      return;
    }
    session = active_session_;
    recording_started_at = recording_started_at_;
    first_non_silent_at = first_non_silent_at_;
  }

  std::vector<vinput::daemon::asr::RecognitionEvent> events;
  std::optional<std::string> failure;
  {
    // Hold state_mutex_ across the poll: StopRecording transfers
    // active_session_ to WorkerMain under the same mutex, so validating
    // identity only after PollEvents() could drain (and then discard)
    // events that WorkerMain still needs for the final result. PollEvents()
    // only briefly locks the session, so this state -> session lock order
    // matches the short critical sections elsewhere and cannot deadlock
    // with the stop path, which never holds session_io_mutex_ while
    // acquiring state_mutex_.
    const std::scoped_lock lock(state_mutex_);
    if (active_session_ != session || phase_ != vinput::dbus::Status::Recording) {
      return;
    }
    recording_started_at = recording_started_at_;
    first_non_silent_at = first_non_silent_at_;
    {
      const std::scoped_lock session_lock(session_io_mutex_);
      events = session->PollEvents();
    }
    // Re-read failure state after polling: a merged wake-up must not lose a
    // backend failure that produced events (or no Error event at all).
    if (auto* async_session =
            dynamic_cast<vinput::daemon::asr::AsyncRecognitionSession*>(session.get())) {
      failure = async_session->FailureError();
    }
    if (failure.has_value()) {
      // FailActiveSession below emits the single error notification; do not
      // also emit one per Error event from the same failure.
      std::erase_if(events, [](const vinput::daemon::asr::RecognitionEvent& event) {
        return event.kind == vinput::daemon::asr::RecognitionEventKind::Error;
      });
    }
    if (!events.empty()) {
      ApplyRecognitionEvents(events, RecognitionDispatchContext{
                                         .latest_final_text = &latest_final_text_,
                                         .first_partial_logged = &first_partial_logged_,
                                         .recording_started_at = recording_started_at,
                                         .first_non_silent_at = first_non_silent_at,
                                         .dbus = dbus_,
                                     });
    }
  }

  if (failure.has_value()) {
    FailActiveSession(session, *failure);
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
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (phase_ != vinput::dbus::Status::Recording) {
      vinput::debug::Log("stop rejected (phase: %s)\n", vinput::dbus::StatusToString(phase_));
      return DbusService::MethodResult::Failure(_("Recording is not active."));
    }
    accepting_chunks_.store(false, std::memory_order_relaxed);
    stop_capture = true;
  }

  if (stop_capture) {
    // StopAndGetBuffer ends recording and deactivates/reuses the stream.
    // Avoid a prior EndRecording() so idle-grace is scheduled once.
    if (const auto first_buffer_ms = capture_->FirstBufferLatencyMs()) {
      vinput::debug::Log("capture first_buffer_ms=%ld (at stop)\n", *first_buffer_ms);
    } else {
      vinput::debug::Log("capture first_buffer_ms=-1 (no buffer before stop)\n");
    }
    captured_pcm = capture_->StopAndGetBuffer();
    RestoreOutputIfDucked();
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!active_session_) {
      vinput::debug::Log("recording stopped without active session\n");
      phase_ = vinput::dbus::Status::Idle;
      current_order_.reset();
      dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Idle));
      vinput::debug::Log("phase -> idle\n");
      apply_pending_reload = true;
    } else {
      if (UsesBufferedDelivery(active_backend_)) {
        // The capture buffer contains raw PCM; callback-side gained PCM is
        // overwritten here. Apply the session's input gain exactly once before
        // passing the authoritative captured samples to the buffered backend.
        vinput::audio::ApplyGainI16(captured_pcm, current_input_gain_);
        current_recording_pcm_ = std::move(captured_pcm);
        current_sample_count_ = current_recording_pcm_.size();
      } else {
        const std::size_t raw_tail_offset = current_sample_count_ + pending_chunk_pcm_.size();
        if (captured_pcm.size() > raw_tail_offset) {
          // Pending chunks already received gain in HandleIncomingAudio. Only
          // the raw tail recovered at Stop needs the same treatment.
          vinput::audio::ApplyGainI16(std::span<int16_t>(captured_pcm).subspan(raw_tail_offset),
                                      current_input_gain_);
          pending_chunk_pcm_.insert(pending_chunk_pcm_.end(),
                                    captured_pcm.begin() +
                                        static_cast<std::ptrdiff_t>(raw_tail_offset),
                                    captured_pcm.end());
        }

        if (!pending_chunk_pcm_.empty()) {
          current_sample_count_ += pending_chunk_pcm_.size();
          vinput::debug::Log("deferred final audio tail chunk to worker samples=%zu captured=%zu "
                             "chunk_size=%zu\n",
                             pending_chunk_pcm_.size(), captured_pcm.size(),
                             kStreamingChunkSamples);
        }
      }

      if (!apply_pending_reload &&
          current_sample_count_ < vinput::daemon::asr::kMinSamplesForRecognition) {
        vinput::debug::Log("recording too short, skipping inference: %zu samples (%.1f ms)\n",
                           current_sample_count_,
                           static_cast<double>(current_sample_count_) * 1000.0 / 16000.0);
        session_to_cancel = ReleaseActiveSessionLocked();
        phase_ = vinput::dbus::Status::Idle;
        current_order_.reset();
        dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Idle));
        vinput::debug::Log("phase -> idle\n");
        apply_pending_reload = true;
      }

      if (!apply_pending_reload) {
        current_order_ = RecognitionOrder{};
        current_order_->audio_delivery_mode = active_backend_.capabilities.audio_delivery_mode;
        current_order_->session = std::move(active_session_);
        current_order_->backend = active_backend_;
        if (UsesBufferedDelivery(active_backend_)) {
          current_order_->pcm = std::move(current_recording_pcm_);
        } else {
          current_order_->pcm = std::move(pending_chunk_pcm_);
        }
        current_order_->recognized_text = latest_final_text_;
        current_order_->scene_id = scene_id;
        current_order_->is_command = current_is_command_;
        current_order_->selected_text = current_selected_text_;
        current_is_command_ = false;
        current_selected_text_.clear();
        LogRecognitionRequest(active_backend_, current_sample_count_);
        current_sample_count_ = 0;
        active_backend_ = {};
        phase_ = vinput::dbus::Status::Inferring;
        dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Inferring));
        worker_cv_.notify_one();
        vinput::debug::Log("recording stopped\n");
      }
    }
  }

  if (session_to_cancel) {
    {
      const std::scoped_lock session_lock(session_io_mutex_);
      session_to_cancel->Cancel();
    }
    // See CancelOperation: retire via the deferred main-thread path instead
    // of joining the async worker here when the local reference is released.
    RetireCancelledSession(std::move(session_to_cancel));
  }

  if (apply_pending_reload) {
    MaybeApplyPendingAsrBackendReload();
  }
  return DbusService::MethodResult::Success();
}

std::shared_ptr<vinput::daemon::asr::RecognitionSession>
DaemonRuntimeController::ReleaseActiveSessionLocked() {
  auto session = std::move(active_session_);
  startup_pcm_buffer_.Clear();
  current_recording_pcm_.clear();
  pending_chunk_pcm_.clear();
  current_sample_count_ = 0;
  current_input_gain_ = 1.0f;
  latest_final_text_.clear();
  recording_started_at_.reset();
  first_non_silent_at_.reset();
  first_partial_logged_ = false;
  active_backend_ = {};
  return session;
}

std::string DaemonRuntimeController::GetStatus() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return vinput::dbus::StatusToString(phase_);
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
  if (remote_text_service_) {
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

  {
    std::vector<std::shared_ptr<vinput::daemon::asr::RecognitionSession>> deferred;
    {
      const std::scoped_lock lock(state_mutex_);
      deferred.swap(deferred_cancelled_sessions_);
    }
    deferred.clear(); // Joins async workers on this (main) thread.
  }

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
    std::lock_guard<std::mutex> lock(state_mutex_);
    phase_snapshot = phase_;
    start_in_progress = start_recording_in_progress_;
    skip_teardown = start_recording_in_progress_ || capture_->IsRecording() ||
                    phase_ == vinput::dbus::Status::Recording ||
                    phase_ == vinput::dbus::Status::Inferring ||
                    phase_ == vinput::dbus::Status::Postprocessing;
  }
  if (skip_teardown) {
    vinput::debug::Log("deferred capture stop skipped (start_in_progress=%d recording=%d "
                       "phase=%s)\n",
                       start_in_progress ? 1 : 0, capture_->IsRecording() ? 1 : 0,
                       vinput::dbus::StatusToString(phase_snapshot));
    RestoreOutputIfDucked();
    return;
  }

  if (AudioCapture::StreamReuseEnabled()) {
    // EndRecording/StopAndGetBuffer already deactivated the stream; keep it
    // for warm set_active until idle grace expires.
    vinput::debug::Log("deferred capture stop: keeping inactive reusable stream\n");
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
  capture_->SetChunkCallback({});
  accepting_chunks_.store(false, std::memory_order_relaxed);
  pending_capture_stop_.store(false, std::memory_order_relaxed);
  capture_->Stop();
  RestoreOutputIfDucked();
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    start_recording_in_progress_ = false;
    session_to_cancel = ReleaseActiveSessionLocked();
    worker_running_ = false;
  }
  if (session_to_cancel) {
    std::lock_guard<std::mutex> session_lock(session_io_mutex_);
    session_to_cancel->Cancel();
  }
  worker_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  std::vector<std::shared_ptr<vinput::daemon::asr::RecognitionSession>> deferred;
  {
    const std::scoped_lock lock(state_mutex_);
    deferred.swap(deferred_cancelled_sessions_);
  }
  deferred.clear();
}

void DaemonRuntimeController::EnterPostprocessing(bool command_mode) {
  {
    const std::scoped_lock lock(state_mutex_);
    postprocessing_state_ =
        command_mode ? PostprocessingState::Command : PostprocessingState::Dictation;
    phase_ = vinput::dbus::Status::Postprocessing;
  }
  dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Postprocessing));
}

DaemonRuntimeController::PostprocessingState DaemonRuntimeController::TakePostprocessingState() {
  const std::scoped_lock lock(state_mutex_);
  const auto state = postprocessing_state_;
  postprocessing_state_ = PostprocessingState::Inactive;
  return state;
}

void DaemonRuntimeController::ResetToIdle() {
  {
    const std::scoped_lock lock(state_mutex_);
    start_recording_in_progress_ = false;
    phase_ = vinput::dbus::Status::Idle;
    postprocessing_state_ = PostprocessingState::Inactive;
    postprocessing_cancel_requested_.store(false, std::memory_order_relaxed);
    current_order_.reset();
    startup_pcm_buffer_.Clear();
    current_is_command_ = false;
    current_selected_text_.clear();
    current_recording_pcm_.clear();
    pending_chunk_pcm_.clear();
    current_sample_count_ = 0;
    current_input_gain_ = 1.0f;
    recording_started_at_.reset();
    first_non_silent_at_.reset();
    first_partial_logged_ = false;
  }
  RestoreOutputIfDucked();
  dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Idle));
  vinput::debug::Log("phase -> idle\n");
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
          const std::lock_guard<std::mutex> session_lock(session_io_mutex_);
          if (!order.session->PushAudio(float_samples, &push_error)) {
            throw std::runtime_error(push_error.empty() ? kFailedToPushAudioError : push_error);
          }
        } else if (!order.pcm.empty()) {
          std::string push_error;
          const std::lock_guard<std::mutex> session_lock(session_io_mutex_);
          if (!order.session->PushAudio(order.pcm, &push_error)) {
            throw std::runtime_error(push_error.empty() ? kFailedToPushAudioError : push_error);
          }
          order.pcm.clear();
        }

        std::string recognition_error;
        auto result =
            FinishSessionAndCollectResult(order.session, &session_io_mutex_, &recognition_error);
        if (!result.ok || !result.error.empty()) {
          // Preserve the structured error classification (as the previous
          // error path did) instead of routing this through the generic
          // exception handler, which would downgrade it to a raw error.
          const std::string failure =
              result.error.empty() ? kFailedToFinishSessionError : result.error;
          fprintf(stderr, "vinput-daemon: recognition error: %s\n", failure.c_str());
          dbus_->EmitNotification(vinput::dbus::ClassifyErrorText(failure));
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
      const auto postprocessing_action = TakePostprocessingState();
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
        if (!error.raw_message.empty()) {
          fprintf(stderr, "vinput-daemon: processing error: %s\n", error.raw_message.c_str());
        }
        dbus_->EmitNotification(error);
      }
      dbus_->EmitRecognitionResult(vinput::result::Serialize(pipeline_result.payload));
    } catch (const std::exception& e) {
      (void)TakePostprocessingState();
      fprintf(stderr, "vinput-daemon: worker exception: %s\n", e.what());
      dbus_->EmitNotification(vinput::dbus::MakeRawError(e.what()));
    } catch (...) {
      (void)TakePostprocessingState();
      fprintf(stderr, "vinput-daemon: worker unknown exception\n");
      dbus_->EmitNotification(vinput::dbus::MakeErrorInfo(
          vinput::dbus::kErrorCodeProcessingUnknown, {}, {}, "Unknown error during processing"));
    }

    ResetToIdle();
  }
}

} // namespace vinput::daemon::runtime
