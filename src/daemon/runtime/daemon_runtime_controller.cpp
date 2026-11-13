#include "daemon/runtime/daemon_runtime_controller.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
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
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/client/client_command.h"
#include "common/client/client_event.h"
#include "common/client/client_manager.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
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
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/recognition_pipeline.h"
#include "daemon/runtime/runtime_state_machine.h"
#include "daemon/runtime/session_lifecycle_manager.h"

namespace vinput::daemon::runtime {

namespace {

template <class... Ts> struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

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
  bool ime = true;
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
  PublishRecognitionText(event.text, ctx);
}

// Partial and final text share one publication path: mirror into the context,
// push the preedit to the IME, then tell every client.
void PublishRecognitionText(const std::string& text, const RecognitionDispatchContext& ctx) {
  if (ctx.latest_partial_text != nullptr) {
    *ctx.latest_partial_text = text;
  }
  if (ctx.ime && ctx.dbus != nullptr) {
    ctx.dbus->EmitRecognitionPartial(text);
  }
  vinput::client::Broadcast(vinput::client::SerializePartialEvent(text, ctx.ime));
}

void HandleFinalText(const vinput::daemon::asr::RecognitionEvent& event,
                     const RecognitionDispatchContext& ctx) {
  if (event.text.empty()) {
    return;
  }
  if (ctx.latest_final_text != nullptr) {
    *ctx.latest_final_text = event.text;
  }
  PublishRecognitionText(event.text, ctx);
}

void HandleError(const vinput::daemon::asr::RecognitionEvent& event,
                 const RecognitionDispatchContext& /*ctx*/) {
  if (event.error.empty()) {
    return;
  }
  VLOG_WARN("recognition event error: {}", event.error);
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
    RecognitionPipeline* pipeline)
    : capture_(capture), dbus_(dbus), recognition_manager_(recognition_manager),
      pipeline_(pipeline) {
  if (recognition_manager_ != nullptr) {
    recognition_manager_->SetReloadResultCallback([this](bool success, const std::string& message) {
      if (success) {
        return;
      }
      if (dbus_ == nullptr || message.empty()) {
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
}

DaemonRuntimeController::~DaemonRuntimeController() {
  Shutdown();
  if (notify_fd_ >= 0) {
    close(notify_fd_);
    notify_fd_ = -1;
  }
}

DbusService::MethodResult DaemonRuntimeController::StartRecording() {
  return StartRecording(true, false, {}, {});
}

DbusService::MethodResult
DaemonRuntimeController::StartCommandRecording(const std::string& selected_text) {
  return StartRecording(true, true, selected_text, {});
}

DbusService::MethodResult
DaemonRuntimeController::StartRecording(bool ime, bool is_command, const std::string& selected_text,
                                        const std::string& capture_device) {
  return StartRecordingInternal(ime, is_command, selected_text, capture_device);
}

DbusService::MethodResult DaemonRuntimeController::StopRecording(const std::string& scene_id) {
  return StopRecording(true, scene_id);
}

DbusService::MethodResult DaemonRuntimeController::CancelOperation(bool commit_raw_text) {
  return CancelOperation(true, commit_raw_text);
}

DbusService::MethodResult DaemonRuntimeController::CancelOperation(bool ime, bool commit_raw_text) {
  bool apply_pending_reload = false;
  bool stop_capture = false;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;

  {
    const std::scoped_lock lock(state_mutex_);
    if (start_recording_in_progress_) {
      start_recording_in_progress_ = false;
      accepting_chunks_.store(false, std::memory_order_relaxed);
      audio_buffer_.Clear();
      stop_capture = true;
      VLOG_DEBUG("in-progress recording start cancelled");
    }

    auto transition = state_machine_.OnCancel(commit_raw_text);
    if (!transition.accepted) {
      if (stop_capture) {
        return DbusService::MethodResult::Success();
      }
      return DbusService::MethodResult::Failure(transition.rejection_reason);
    }
    session_ime_ = ime;

    switch (transition.action) {
    case TransitionAction::AbortCapture:
      accepting_chunks_.store(false, std::memory_order_relaxed);
      stop_capture = true;
      ResetRecordingStateLocked();
      session_to_cancel = session_lifecycle_.DetachActive();
      current_order_.reset();
      NotifyStatusChanged(transition.new_phase);
      VLOG_DEBUG("recording cancelled and discarded");
      apply_pending_reload = pending_asr_backend_reload_;
      break;
    case TransitionAction::RequestCommitRaw:
    case TransitionAction::RequestDiscard:
      postprocessing_cancel_requested_.store(true, std::memory_order_relaxed);
      VLOG_DEBUG("post-processing cancellation requested action={}",
                 commit_raw_text ? "commit-raw" : "discard");
      break;
    default:
      break;
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
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

DbusService::MethodResult DaemonRuntimeController::ReloadAsrBackend() {
  {
    const std::scoped_lock lock(state_mutex_);
    if (!state_machine_.IsIdle() || start_recording_in_progress_) {
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
    if (!pending_asr_backend_reload_ || !state_machine_.IsIdle()) {
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

void DaemonRuntimeController::NotifyStatusChanged(vinput::dbus::Status status) {
  if (session_ime_ && dbus_ != nullptr) {
    dbus_->EmitStatusChanged(vinput::dbus::StatusToString(status));
  }
  vinput::client::Broadcast(
      vinput::client::SerializeStatusEvent(vinput::dbus::StatusToString(status), session_ime_));
}

DbusService::MethodResult
DaemonRuntimeController::StartRecordingInternal(bool ime, bool is_command,
                                                const std::string& selected_text,
                                                const std::string& capture_device) {
  const auto start_enter_at = std::chrono::steady_clock::now();
  // Drain any abort-path deferred stop before arming a new recording so a
  // rapid Tap-stop-Tap cannot destroy a just-reused stream.
  if (pending_capture_stop_.load(std::memory_order_acquire)) {
    FlushDeferredActions();
  }

  {
    const std::scoped_lock lock(state_mutex_);
    if (!TryBeginRecordingStartupLocked(ime)) {
      VLOG_DEBUG("start rejected (phase: {} start_in_progress={})",
                 vinput::dbus::StatusToString(state_machine_.CurrentPhase()),
                 start_recording_in_progress_);
      return DbusService::MethodResult::Failure(
          _("Voice input daemon is busy. Please wait for the current request to finish."));
    }
  }

  auto runtime_settings = LoadCoreConfig();
  NormalizeCoreConfig(&runtime_settings);

  // Open the mic before CreateSession so ASR session setup cannot delay the
  // first capture buffers after a cold press. Chunks are only accepted after
  // both capture and session are armed below.
  std::string capture_error;
  AudioCapture::StartTiming capture_timing;
  if (!PrepareAndBeginCapture(capture_device, runtime_settings, is_command, &capture_timing,
                              &capture_error)) {
    {
      const std::scoped_lock lock(state_mutex_);
      AbortRecordingStartupLocked();
    }
    VLOG_WARN("{}", capture_error);
    return DbusService::MethodResult::Failure(capture_error);
  }

  vinput::daemon::asr::BackendDescriptor active_backend;
  std::string session_error;
  const auto session_start_at = std::chrono::steady_clock::now();
  auto session =
      recognition_manager_->CreateSession(runtime_settings, &active_backend, &session_error);
  const long session_ms = static_cast<long>(vinput::time::MillisecondsSince(session_start_at));
  if (!session) {
    std::string message = _("Failed to start recognition session.");
    if (!session_error.empty()) {
      message += " " + session_error;
    }
    AbortAndScheduleCaptureStop();
    VLOG_WARN("{}", message);
    return DbusService::MethodResult::Failure(message);
  }

  session = WrapAsyncSessionIfNeeded(std::move(session), active_backend);

  bool abort_started_capture = false;
  {
    const std::scoped_lock lock(state_mutex_);
    if (!start_recording_in_progress_ || !state_machine_.IsIdle()) {
      AbortRecordingStartupLocked();
      abort_started_capture = true;
    } else {
      ArmLiveRecordingLocked(std::move(session), active_backend, runtime_settings, is_command,
                             selected_text);
    }
  }

  if (abort_started_capture) {
    AbortAndScheduleCaptureStop();
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
             is_command, session_ms, capture_timing.idle_gap_ms, capture_timing.create_stream_ms,
             capture_timing.set_active_ms, capture_timing.stream_reused,
             capture_timing.reuse_policy_enabled, start_total_ms, active_backend.backend_id);

  NotifyStatusChanged(vinput::dbus::Status::Recording);
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

  if (streaming_mode) {
    PushStreamingChunks(session, std::move(chunk_to_push));
  }
}

void DaemonRuntimeController::RestoreOutputIfDucked() {
  output_ducker_.Restore();
}

bool DaemonRuntimeController::TryBeginRecordingStartupLocked(bool ime) {
  if (!state_machine_.IsIdle() || start_recording_in_progress_) {
    return false;
  }
  pending_capture_stop_.store(false, std::memory_order_release);
  audio_buffer_.Reset();
  session_ime_ = ime;
  start_recording_in_progress_ = true;
  accepting_chunks_.store(true, std::memory_order_relaxed);
  return true;
}

void DaemonRuntimeController::AbortAndScheduleCaptureStop() {
  capture_->EndRecording();
  {
    const std::scoped_lock lock(state_mutex_);
    AbortRecordingStartupLocked();
  }
  RestoreOutputIfDucked();
  ScheduleCaptureStopOnMainThread();
}

void DaemonRuntimeController::AbortRecordingStartupLocked() {
  start_recording_in_progress_ = false;
  accepting_chunks_.store(false, std::memory_order_relaxed);
  audio_buffer_.Clear();
}

void DaemonRuntimeController::ResetRecordingStateLocked() {
  audio_buffer_.Clear();
  latest_final_text_.clear();
  recording_started_at_.reset();
  first_non_silent_at_.reset();
  first_partial_logged_ = false;
  active_backend_ = {};
}

void DaemonRuntimeController::ArmLiveRecordingLocked(
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> session,
    const vinput::daemon::asr::BackendDescriptor& active_backend,
    const CoreConfig& runtime_settings, bool is_command, const std::string& selected_text) {
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

std::unique_ptr<vinput::daemon::asr::RecognitionSession>
DaemonRuntimeController::WrapAsyncSessionIfNeeded(
    std::unique_ptr<vinput::daemon::asr::RecognitionSession> session,
    const vinput::daemon::asr::BackendDescriptor& active_backend) const {
  if (active_backend.capabilities.audio_delivery_mode !=
      vinput::daemon::asr::AudioDeliveryMode::Chunked) {
    return session;
  }
  auto async_session =
      std::make_unique<vinput::daemon::asr::AsyncRecognitionSession>(std::move(session));
  async_session->SetWakeCallback([notify_fd = notify_fd_] {
    if (notify_fd >= 0) {
      const uint64_t value = 1;
      (void)write(notify_fd, &value, sizeof(value));
    }
  });
  return async_session;
}

void DaemonRuntimeController::PushStreamingChunks(
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> session,
    std::optional<std::vector<int16_t>> chunk_to_push) {
  while (chunk_to_push.has_value()) {
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

bool DaemonRuntimeController::PrepareAndBeginCapture(const std::string& capture_device,
                                                     const CoreConfig& runtime_settings,
                                                     bool is_command,
                                                     AudioCapture::StartTiming* capture_timing,
                                                     std::string* error_message) {
  std::string error;
  const std::string target_device =
      capture_device.empty() ? runtime_settings.global.captureDevice : capture_device;
  capture_->SetTargetObject(target_device);
  capture_->SetNotifyFd(notify_fd_);
  if (!capture_->BeginRecording(&error, capture_timing)) {
    std::string message =
        is_command ? _("Failed to start command recording.") : _("Failed to start recording.");
    if (!error.empty()) {
      message = std::string(vinput::str::StripSuffix(message, ".")) + ": " + error;
    }
    if (error_message != nullptr) {
      *error_message = std::move(message);
    }
    return false;
  }
  return true;
}

void DaemonRuntimeController::FeedOrderAudio(RecognitionOrder& order, bool normalize_audio) {
  if (order.pcm.empty()) {
    return;
  }

  if (normalize_audio &&
      order.audio_delivery_mode == vinput::daemon::asr::AudioDeliveryMode::Buffered) {
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
    return;
  }

  std::string push_error;
  if (!session_lifecycle_.PushAudio(order.session, order.pcm, &push_error)) {
    throw std::runtime_error(push_error);
  }
  order.pcm.clear();
}

std::vector<int16_t> DaemonRuntimeController::StopCaptureAndDrainMetrics() {
  if (const auto first_buffer_ms = capture_->FirstBufferLatencyMs()) {
    VLOG_DEBUG("capture first_buffer_ms={} (at stop)", *first_buffer_ms);
  } else {
    VLOG_DEBUG("capture first_buffer_ms=-1 (no buffer before stop)");
  }
  const auto dropped_chunks = capture_->DroppedChunks();
  const auto dropped_samples = capture_->DroppedSamples();
  if (dropped_chunks > 0 || dropped_samples > 0) {
    VLOG_WARN("capture audio queue dropped {} chunks ({} samples) due to overflow", dropped_chunks,
              dropped_samples);
  }
  auto captured = capture_->StopAndGetBuffer();
  RestoreOutputIfDucked();
  return captured;
}

void DaemonRuntimeController::ScheduleInferenceLocked(const std::string& scene_id,
                                                      std::size_t total_samples,
                                                      std::size_t captured_samples,
                                                      vinput::dbus::Status next_phase) {
  current_order_ = RecognitionOrder{};
  current_order_->audio_delivery_mode = active_backend_.capabilities.audio_delivery_mode;
  current_order_->session = session_lifecycle_.DetachActive();
  current_order_->backend = active_backend_;
  current_order_->ime = session_ime_;
  if (UsesBufferedDelivery(active_backend_)) {
    current_order_->pcm = audio_buffer_.TakeBufferedPcm();
  } else {
    current_order_->pcm = audio_buffer_.DrainStreamingTail();
    if (!current_order_->pcm.empty()) {
      VLOG_DEBUG("deferred final audio tail chunk to worker samples={} captured={} chunk_size={}",
                 current_order_->pcm.size(), captured_samples,
                 AudioRecordingBuffer::kStreamingChunkSamples);
    }
  }
  current_order_->postprocess_request = PostprocessRequest{
      .text = latest_final_text_,
      .scene_id = scene_id,
      .is_command = state_machine_.IsCommand(),
      .selected_text = state_machine_.SelectedText(),
  };
  LogRecognitionRequest(active_backend_, total_samples);
  active_backend_ = {};
  NotifyStatusChanged(next_phase);
  worker_cv_.notify_one();
  VLOG_DEBUG("recording stopped");
}

bool DaemonRuntimeController::ExecuteRecognitionSession(RecognitionOrder& order,
                                                        const CoreConfig& runtime_settings) {
  if (!order.session) {
    return true;
  }

  FeedOrderAudio(order, runtime_settings.asr.normalizeAudio);

  std::string recognition_error;
  auto result = session_lifecycle_.FinishSessionAndCollectResult(order.session, &recognition_error);
  if (!result.ok || !result.error.empty()) {
    const std::string failure =
        result.error.empty() ? "Failed to finish recognition session." : result.error;
    VLOG_ERROR("recognition error: {}", failure);
    dbus_->EmitDesktopNotification(vinput::daemon::notify::MakeDaemonErrorNotification(failure));
    return false;
  }

  if (!result.text.empty()) {
    order.postprocess_request.text = std::move(result.text);
    if (order.ime && dbus_ != nullptr) {
      dbus_->EmitRecognitionPartial(order.postprocess_request.text);
    }
    vinput::client::Broadcast(
        vinput::client::SerializePartialEvent(order.postprocess_request.text, order.ime));
  }
  order.pcm.clear();
  order.session.reset();
  return true;
}

void DaemonRuntimeController::ExecutePostprocessingAndBroadcast(
    RecognitionOrder& order, const CoreConfig& runtime_settings) {
  postprocessing_cancel_requested_.store(false, std::memory_order_relaxed);
  auto pipeline_result = pipeline_->Process(
      order.postprocess_request, runtime_settings,
      [&]() { EnterPostprocessing(order.postprocess_request.is_command); },
      &postprocessing_cancel_requested_);
  PostprocessingState postprocessing_action = PostprocessingState::Inactive;
  {
    const std::scoped_lock lock(state_mutex_);
    postprocessing_action = state_machine_.TakePostprocessingState();
  }
  if (postprocessing_action == PostprocessingState::CommitRaw) {
    pipeline_result.payload.commitText = order.postprocess_request.text;
    pipeline_result.payload.candidates = {
        {.text = order.postprocess_request.text, .source = vinput::result::kSourceRaw}};
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
  if (order.ime && dbus_ != nullptr) {
    dbus_->EmitRecognitionResult(vinput::result::Serialize(pipeline_result.payload));
  }
  if (!pipeline_result.payload.commitText.empty()) {
    vinput::client::Broadcast(
        vinput::client::SerializeCommittedEvent(pipeline_result.payload, order.ime));
  }
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

  NotifyStatusChanged(vinput::dbus::Status::Error);
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
    if (!state_machine_.IsRecording() || !session_lifecycle_.HasActiveSession() ||
        UsesBufferedDelivery(active_backend_)) {
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
                                              .ime = session_ime_,
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

DbusService::MethodResult DaemonRuntimeController::StopRecording(bool ime,
                                                                 const std::string& scene_id) {
  bool stop_capture = false;
  {
    const std::scoped_lock lock(state_mutex_);
    if (!state_machine_.IsRecording()) {
      VLOG_DEBUG("stop rejected (phase: {})",
                 vinput::dbus::StatusToString(state_machine_.CurrentPhase()));
      return DbusService::MethodResult::Failure(_("Recording is not active."));
    }
    session_ime_ = ime;
    accepting_chunks_.store(false, std::memory_order_relaxed);
    stop_capture = true;
  }

  std::vector<int16_t> captured_pcm;
  if (stop_capture) {
    captured_pcm = StopCaptureAndDrainMetrics();
  }

  bool apply_pending_reload = false;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;
  {
    const std::scoped_lock lock(state_mutex_);
    if (!session_lifecycle_.HasActiveSession()) {
      VLOG_DEBUG("recording stopped without active session");
      state_machine_.OnResetToIdle();
      current_order_.reset();
      NotifyStatusChanged(vinput::dbus::Status::Idle);
      VLOG_DEBUG("phase -> idle");
      apply_pending_reload = true;
    } else {
      if (!captured_pcm.empty()) {
        audio_buffer_.Append(captured_pcm);
      }

      const auto total_samples = audio_buffer_.TotalSampleCount();
      auto transition = state_machine_.OnStopRecording(
          total_samples, vinput::daemon::asr::kMinSamplesForRecognition);

      if (transition.action == TransitionAction::DiscardTooShort) {
        VLOG_DEBUG("recording too short, skipping inference: {} samples ({:.1f} ms)", total_samples,
                   static_cast<double>(total_samples) * 1000.0 / 16000.0);
        ResetRecordingStateLocked();
        session_to_cancel = session_lifecycle_.DetachActive();
        current_order_.reset();
        NotifyStatusChanged(transition.new_phase);
        VLOG_DEBUG("phase -> idle");
        apply_pending_reload = true;
      } else {
        ScheduleInferenceLocked(scene_id, total_samples, captured_pcm.size(), transition.new_phase);
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
  bool ime = true;
  {
    const std::scoped_lock lock(state_mutex_);
    const auto res = state_machine_.OnInferenceComplete(true);
    next_phase = res.new_phase;
    ime = session_ime_;
  }
  if (ime && dbus_ != nullptr) {
    dbus_->EmitStatusChanged(vinput::dbus::StatusToString(next_phase));
  }
  vinput::client::Broadcast(
      vinput::client::SerializeStatusEvent(vinput::dbus::StatusToString(next_phase), ime));
}

void DaemonRuntimeController::ResetToIdle() {
  bool was_ime = true;
  {
    const std::scoped_lock lock(state_mutex_);
    start_recording_in_progress_ = false;
    postprocessing_cancel_requested_.store(false, std::memory_order_relaxed);
    state_machine_.OnResetToIdle();
    current_order_.reset();
    ResetRecordingStateLocked();
    was_ime = session_ime_;
    session_ime_ = true;
  }
  RestoreOutputIfDucked();
  if (was_ime && dbus_ != nullptr) {
    dbus_->EmitStatusChanged(vinput::dbus::StatusToString(vinput::dbus::Status::Idle));
  }
  vinput::client::Broadcast(vinput::client::SerializeStatusEvent(
      vinput::dbus::StatusToString(vinput::dbus::Status::Idle), was_ime));
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
      auto runtime_settings = LoadCoreConfig();
      NormalizeCoreConfig(&runtime_settings);

      if (!ExecuteRecognitionSession(order, runtime_settings)) {
        ResetToIdle();
        continue;
      }

      ExecutePostprocessingAndBroadcast(order, runtime_settings);
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

DbusService::MethodResult DaemonRuntimeController::InjectText(bool ime,
                                                              vinput::client::InjectStage stage,
                                                              const std::string& text,
                                                              const std::string& scene_id) {
  if (stage == vinput::client::InjectStage::Partial) {
    if (ime && dbus_ != nullptr) {
      dbus_->EmitRecognitionPartial(text);
    }
    vinput::client::Broadcast(vinput::client::SerializePartialEvent(text, ime));
    return DbusService::MethodResult::Success();
  }

  // Final stage: if no scene is requested, commit text directly without worker thread
  if (scene_id.empty()) {
    vinput::result::Payload payload;
    payload.commitText = text;
    payload.candidates = {{.text = text, .source = vinput::result::kSourceRaw}};
    if (ime && dbus_ != nullptr) {
      dbus_->EmitRecognitionResult(vinput::result::Serialize(payload));
    }
    if (!text.empty()) {
      vinput::client::Broadcast(vinput::client::SerializeCommittedEvent(payload, ime));
    }
    return DbusService::MethodResult::Success();
  }

  // Queue on worker thread for asynchronous scene post-processing
  {
    const std::scoped_lock lock(state_mutex_);
    if (!state_machine_.IsIdle()) {
      return DbusService::MethodResult::Failure(
          _("Voice input daemon is busy. Please wait for the current request to finish."));
    }
    session_ime_ = ime;
    RecognitionOrder order;
    order.postprocess_request = PostprocessRequest{
        .text = text,
        .scene_id = scene_id,
        .is_command = false,
        .selected_text = {},
    };
    order.ime = ime;
    current_order_ = std::move(order);
  }
  worker_cv_.notify_one();
  return DbusService::MethodResult::Success();
}

void DaemonRuntimeController::DispatchClientCommand(const vinput::client::ClientCommand& command) {
  const auto result = std::visit(Overloaded{
                                     [this](const vinput::client::StartCommand& cmd) {
                                       const bool is_command = (cmd.mode == "command");
                                       return StartRecording(cmd.ime, is_command, cmd.selected_text,
                                                             cmd.capture_device);
                                     },
                                     [this](const vinput::client::StopCommand& cmd) {
                                       return StopRecording(cmd.ime, cmd.scene);
                                     },
                                     [this](const vinput::client::CancelCommand& cmd) {
                                       return CancelOperation(cmd.ime, cmd.commit_raw);
                                     },
                                     [this](const vinput::client::InjectCommand& cmd) {
                                       return InjectText(cmd.ime, cmd.stage, cmd.text, cmd.scene);
                                     },
                                 },
                                 command);
  if (!result.ok) {
    VLOG_WARN("client command rejected: {}", result.message);
  }
}

void DaemonRuntimeController::DispatchClientLine(std::string_view line) {
  std::string err;
  const auto cmd = vinput::client::ParseClientCommand(line, &err);
  if (!cmd.has_value()) {
    VLOG_WARN("invalid client command: {}", err);
    return;
  }
  DispatchClientCommand(*cmd);
}

} // namespace vinput::daemon::runtime
