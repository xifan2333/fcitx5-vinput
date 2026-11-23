#include "daemon/runtime/session_lane.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/client/client_command.h"
#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/utils/debug_log.h"
#include "common/utils/time_utils.h"
#include "common/utils/unique_fd.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/audio_utils.h"
#include "daemon/notify/desktop_notification_sender.h"
#include "daemon/runtime/audio_recording_buffer.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/recognition_pipeline.h"

namespace vinput::daemon::runtime {

namespace {

bool UsesBufferedDelivery(const vinput::daemon::asr::BackendDescriptor& descriptor) noexcept {
  return descriptor.capabilities.audio_delivery_mode ==
         vinput::daemon::asr::AudioDeliveryMode::Buffered;
}

long DeltaMs(const std::optional<SessionLane::TimePoint>& origin,
             const std::optional<SessionLane::TimePoint>& target) noexcept {
  if (!origin.has_value() || !target.has_value()) {
    return -1;
  }
  return static_cast<long>(vinput::time::MillisecondsBetween(*origin, *target));
}

void CollectFinishEvents(const std::vector<vinput::daemon::asr::RecognitionEvent>& events,
                         std::string& final_text, std::string& error_out, bool& ok) {
  for (const auto& event : events) {
    switch (event.kind) {
    case vinput::daemon::asr::RecognitionEventKind::PartialText:
    case vinput::daemon::asr::RecognitionEventKind::Completed:
      break;
    case vinput::daemon::asr::RecognitionEventKind::FinalText:
      final_text = event.text;
      break;
    case vinput::daemon::asr::RecognitionEventKind::Error:
      ok = false;
      if (!event.error.empty()) {
        error_out = event.error;
      }
      break;
    }
  }
}

bool PushBufferedNormalizedPcm(vinput::daemon::asr::RecognitionSession& session,
                               std::vector<int16_t>& pcm, std::string* error_out) {
  std::vector<float> float_samples(pcm.size());
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    float_samples[i] = static_cast<float>(pcm[i]) / 32768.0F;
  }
  pcm.clear();
  pcm.shrink_to_fit();
  vinput::audio::PeakNormalize(float_samples);
  return session.PushAudio(float_samples, error_out);
}

bool PushChunkedPcm(vinput::daemon::asr::RecognitionSession& session, std::vector<int16_t>& pcm,
                    std::string* error_out) {
  const std::span<const int16_t> all(pcm);
  for (std::size_t offset = 0; offset < all.size();
       offset += AudioRecordingBuffer::kStreamingChunkSamples) {
    const std::size_t count =
        std::min(AudioRecordingBuffer::kStreamingChunkSamples, all.size() - offset);
    if (!session.PushAudio(all.subspan(offset, count), error_out)) {
      pcm.clear();
      return false;
    }
  }
  pcm.clear();
  return true;
}

bool PushOrderPcm(vinput::daemon::asr::RecognitionSession& session, std::vector<int16_t>& pcm,
                  vinput::daemon::asr::AudioDeliveryMode mode, bool normalize_audio,
                  std::string* error_out) {
  if (pcm.empty()) {
    return true;
  }
  std::string push_error;
  bool pushed = false;
  if (normalize_audio && mode == vinput::daemon::asr::AudioDeliveryMode::Buffered) {
    pushed = PushBufferedNormalizedPcm(session, pcm, &push_error);
  } else if (mode == vinput::daemon::asr::AudioDeliveryMode::Chunked) {
    pushed = PushChunkedPcm(session, pcm, &push_error);
  } else {
    pushed = session.PushAudio(pcm, &push_error);
    pcm.clear();
  }
  if (!pushed && error_out != nullptr) {
    *error_out = push_error.empty() ? _("Failed to push audio chunk.") : std::move(push_error);
  }
  return pushed;
}

} // namespace

SessionLane::SessionLane(SessionLaneDependencies deps) : deps_(std::move(deps)) {
  if (deps_.shared_notify_fd >= 0) {
    notify_fd_ = deps_.shared_notify_fd;
  } else {
    owned_notify_fd_.Reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    notify_fd_ = owned_notify_fd_.Get();
    if (!owned_notify_fd_.IsValid()) {
      VLOG_ERROR("failed to create session lane notify fd: {}", std::strerror(errno));
    }
  }
  if (deps_.audio_source != nullptr && notify_fd_ >= 0) {
    deps_.audio_source->SetNotifyFd(notify_fd_);
  }
}

SessionLane::~SessionLane() {
  Shutdown();
}

void SessionLane::WakeMainThread() const noexcept {
  vinput::file::SignalEventFd(notify_fd_);
}

void SessionLane::PostInboxEvent(InboxEvent event) {
  {
    const std::scoped_lock lock(inbox_mutex_);
    inbox_.push_back(std::move(event));
  }
  WakeMainThread();
}

void SessionLane::NotifySourceEof(std::uint64_t token) {
  PostInboxEvent(SourceEofEvent{.token = token});
}

std::uint64_t SessionLane::CurrentToken() const noexcept {
  return attempt_.token;
}

LanePhase SessionLane::CurrentPhase() const noexcept {
  return phase_;
}

vinput::dbus::Status SessionLane::CurrentDbusStatus() const noexcept {
  switch (phase_) {
  case LanePhase::Idle:
    return vinput::dbus::Status::Idle;
  case LanePhase::Starting:
    return vinput::dbus::Status::Starting;
  case LanePhase::Recording:
    return vinput::dbus::Status::Recording;
  case LanePhase::Stopping:
    return vinput::dbus::Status::Stopping;
  case LanePhase::Inferring:
    return vinput::dbus::Status::Inferring;
  case LanePhase::Postprocessing:
    return vinput::dbus::Status::Postprocessing;
  }
  return vinput::dbus::Status::Idle;
}

std::string SessionLane::GetStatus() const {
  return vinput::dbus::StatusToString(CurrentDbusStatus());
}

bool SessionLane::IsIdle() const noexcept {
  return phase_ == LanePhase::Idle;
}

bool SessionLane::BeginAudioCapture(std::uint64_t token, const std::string& target, bool is_command,
                                    std::string* error_out) {
  if (deps_.audio_source == nullptr) {
    return true;
  }
  deps_.audio_source->SetNotifyFd(notify_fd_);
  std::string capture_error;
  auto on_first = [this, token](int64_t ts_ns) {
    PostInboxEvent(FirstBufferEvent{.token = token, .timestamp_ns = ts_ns});
  };
  auto on_err = [this, token](std::string_view err) {
    PostInboxEvent(AudioErrorEvent{.token = token, .error = std::string(err)});
  };
  if (deps_.audio_source->Begin(target, std::move(on_first), std::move(on_err), &capture_error)) {
    return true;
  }
  std::string message =
      is_command ? _("Failed to start command recording.") : _("Failed to start recording.");
  if (!capture_error.empty()) {
    message += ": " + capture_error;
  }
  VLOG_WARN("{}", message);
  if (error_out != nullptr) {
    *error_out = std::move(message);
  }
  return false;
}

DbusService::MethodResult SessionLane::Start(bool ime, bool is_command,
                                             const std::string& selected_text,
                                             const std::string& capture_device,
                                             const std::string& default_scene_id) {
  {
    const std::scoped_lock lock(worker_mutex_);
    if (shutting_down_) {
      return DbusService::MethodResult::Failure(_("daemon is shutting down"));
    }
  }
  if (phase_ != LanePhase::Idle) {
    VLOG_DEBUG("start rejected (phase: {})", GetStatus());
    return DbusService::MethodResult::Failure(
        _("Voice input daemon is busy. Please wait for the current request to finish."));
  }

  CoreConfig settings = deps_.load_config ? deps_.load_config() : CoreConfig{};
  const std::uint64_t token = ++next_token_;
  const TimePoint start_at = Clock::now();

  audio_buffer_.Reset(static_cast<float>(settings.asr.inputGain));

  const std::string target =
      capture_device.empty() ? settings.global.captureDevice : capture_device;
  std::string capture_error;
  if (!BeginAudioCapture(token, target, is_command, &capture_error)) {
    return DbusService::MethodResult::Failure(capture_error);
  }

  attempt_ = ActiveAttempt{};
  attempt_.token = token;
  attempt_.ime = ime;
  attempt_.is_command = is_command;
  attempt_.selected_text = selected_text;
  attempt_.default_scene_id = default_scene_id;
  attempt_.settings = settings;
  attempt_.start_received_at = start_at;
  attempt_.connect_returned_at = Clock::now();
  phase_ = LanePhase::Starting;

  if (ime && settings.global.duckOutputWhileRecording && deps_.output_ducker != nullptr) {
    deps_.output_ducker->Duck(settings.global.duckOutputVolume);
    attempt_.ducked = true;
  }

  if (deps_.event_sink != nullptr) {
    deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Starting, ime);
  }

  EnsureWorkerStarted();
  SubmitWorkerTask(CreateAsrSessionTask{.token = token, .settings = std::move(settings)});
  return DbusService::MethodResult::Success();
}

DbusService::MethodResult SessionLane::Stop(bool ime, const std::string& scene_id) {
  if (phase_ == LanePhase::Starting) {
    attempt_.ime = ime;
    attempt_.pending_stop = PendingStop{.ime = ime, .scene_id = scene_id, .stop_at = Clock::now()};
    attempt_.stop_at = attempt_.pending_stop->stop_at;
    phase_ = LanePhase::Stopping;
    if (deps_.event_sink != nullptr) {
      deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Stopping, attempt_.ime);
    }
    MaybeCompleteReadiness();
    return DbusService::MethodResult::Success();
  }

  if (phase_ == LanePhase::Recording) {
    attempt_.ime = ime;
    attempt_.pending_stop = PendingStop{.ime = ime, .scene_id = scene_id, .stop_at = Clock::now()};
    attempt_.stop_at = attempt_.pending_stop->stop_at;
    phase_ = LanePhase::Stopping;
    FinishCaptureAndSubmitOrDiscard();
    return DbusService::MethodResult::Success();
  }

  VLOG_DEBUG("stop rejected (phase: {})", GetStatus());
  return DbusService::MethodResult::Failure(_("Recording is not active."));
}

DbusService::MethodResult SessionLane::Cancel(bool ime, bool commit_raw_text) {
  switch (phase_) {
  case LanePhase::Idle:
    return DbusService::MethodResult::Success();
  case LanePhase::Starting:
  case LanePhase::Recording:
  case LanePhase::Stopping:
    attempt_.ime = ime;
    if (deps_.audio_source != nullptr) {
      deps_.audio_source->Abort();
    }
    RestoreDuckerIfNeeded();
    RetireSession(std::move(attempt_.asr_session));
    TransitionToIdle(true);
    VLOG_DEBUG("recording cancelled and discarded");
    return DbusService::MethodResult::Success();
  case LanePhase::Inferring:
    return DbusService::MethodResult::Failure(_("No cancellable operation in current phase."));
  case LanePhase::Postprocessing: {
    const bool allowed = !attempt_.is_command || !commit_raw_text;
    if (!allowed) {
      return DbusService::MethodResult::Failure(
          _("Requested post-processing action is not available."));
    }
    attempt_.ime = ime;
    attempt_.postprocessing_action =
        commit_raw_text ? PostprocessingAction::CommitRaw : PostprocessingAction::Discard;
    if (attempt_.cancel_flag) {
      attempt_.cancel_flag->store(true, std::memory_order_release);
    }
    VLOG_DEBUG("post-processing cancellation requested action={}",
               commit_raw_text ? "commit-raw" : "discard");
    return DbusService::MethodResult::Success();
  }
  }
  return DbusService::MethodResult::Failure(_("No cancellable operation in current phase."));
}

DbusService::MethodResult SessionLane::InjectText(bool ime, vinput::client::InjectStage stage,
                                                  const std::string& text,
                                                  const std::string& scene_id) {
  if (stage == vinput::client::InjectStage::Partial) {
    if (deps_.event_sink != nullptr) {
      deps_.event_sink->OnPartialText(text, ime);
    }
    return DbusService::MethodResult::Success();
  }

  if (scene_id.empty()) {
    vinput::result::Payload payload;
    payload.commitText = text;
    payload.candidates = {{.text = text, .source = vinput::result::kSourceRaw}};
    if (deps_.event_sink != nullptr) {
      deps_.event_sink->OnRecognitionResult(payload, ime);
    }
    return DbusService::MethodResult::Success();
  }

  if (phase_ != LanePhase::Idle) {
    return DbusService::MethodResult::Failure(
        _("Voice input daemon is busy. Please wait for the current request to finish."));
  }
  {
    const std::scoped_lock lock(worker_mutex_);
    if (shutting_down_) {
      return DbusService::MethodResult::Failure(_("daemon is shutting down"));
    }
  }

  CoreConfig settings = deps_.load_config ? deps_.load_config() : CoreConfig{};
  const std::uint64_t token = ++next_token_;
  attempt_ = ActiveAttempt{};
  attempt_.token = token;
  attempt_.ime = ime;
  attempt_.is_command = false;
  attempt_.latest_final_text = text;
  attempt_.settings = settings;
  attempt_.cancel_flag = std::make_shared<std::atomic<bool>>(false);
  phase_ = LanePhase::Postprocessing;

  if (deps_.event_sink != nullptr) {
    deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Postprocessing, ime);
  }

  EnsureWorkerStarted();
  SubmitWorkerTask(ProcessOrderTask{
      .token = token,
      .audio_delivery_mode = vinput::daemon::asr::AudioDeliveryMode::Buffered,
      .session = nullptr,
      .backend = {},
      .pcm = {},
      .postprocess_request =
          PostprocessRequest{
              .text = text,
              .scene_id = scene_id,
              .is_command = false,
              .selected_text = {},
          },
      .settings = std::move(settings),
      .ime = ime,
      .cancel_flag = attempt_.cancel_flag,
  });
  return DbusService::MethodResult::Success();
}

void SessionLane::ProcessPendingEvents() {
  FlushRetiredSessions();

  bool progressed = true;
  while (progressed) {
    progressed = DrainInbox();
    if (phase_ == LanePhase::Starting || phase_ == LanePhase::Recording ||
        phase_ == LanePhase::Stopping) {
      DrainIncomingAudio();
      if (DrainInbox()) {
        progressed = true;
      }
    }
  }

  CheckReadinessTimeout();
  PollActiveSessionEvents();
  FlushRetiredSessions();
}

int SessionLane::NextTimeoutMs() const {
  if ((phase_ != LanePhase::Starting && phase_ != LanePhase::Stopping) ||
      attempt_.first_buffer_received || attempt_.first_buffer_timed_out || attempt_.source_eof ||
      !attempt_.connect_returned_at.has_value()) {
    return -1;
  }
  const auto deadline = *attempt_.connect_returned_at + deps_.first_buffer_ready_timeout;
  const auto now = Clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<int>(std::max<std::chrono::milliseconds::rep>(1, remaining.count()));
}

bool SessionLane::DrainInbox() {
  std::deque<InboxEvent> local;
  {
    const std::scoped_lock lock(inbox_mutex_);
    if (inbox_.empty()) {
      return false;
    }
    local.swap(inbox_);
  }
  for (auto& event : local) {
    HandleInboxEvent(event);
  }
  return true;
}

std::optional<std::string> SessionLane::PendingTerminalAudioError() const {
  const std::scoped_lock lock(inbox_mutex_);
  for (const auto& event : inbox_) {
    if (const auto* err = std::get_if<AudioErrorEvent>(&event);
        err != nullptr && err->token == attempt_.token) {
      return err->error;
    }
  }
  return std::nullopt;
}

void SessionLane::HandleInboxEvent(InboxEvent& event) {
  if (auto* fb = std::get_if<FirstBufferEvent>(&event)) {
    HandleFirstBuffer(*fb);
  } else if (auto* err = std::get_if<AudioErrorEvent>(&event)) {
    HandleAudioError(*err);
  } else if (auto* eof = std::get_if<SourceEofEvent>(&event)) {
    HandleSourceEof(*eof);
  } else if (auto* ready = std::get_if<AsrSessionReadyEvent>(&event)) {
    HandleAsrSessionReady(*ready);
  } else if (auto* text = std::get_if<RecognizedTextEvent>(&event)) {
    HandleRecognizedText(*text);
  } else if (auto* post = std::get_if<EnterPostprocessingEvent>(&event)) {
    HandleEnterPostprocessing(*post);
  } else if (auto* done = std::get_if<WorkerCompletedEvent>(&event)) {
    HandleWorkerCompleted(*done);
  }
}

void SessionLane::HandleFirstBuffer(const FirstBufferEvent& ev) {
  if (ev.token != attempt_.token) {
    return;
  }
  // Audio sources stamp the first buffer in the same steady-clock domain as Clock.
  // Late events after a readiness timeout only fill in the metric and must not
  // re-advance readiness.
  if (!attempt_.first_buffer_at.has_value() && ev.timestamp_ns > 0) {
    attempt_.first_buffer_at = TimePoint(std::chrono::nanoseconds(ev.timestamp_ns));
  }
  if (attempt_.first_buffer_received ||
      (phase_ != LanePhase::Starting && phase_ != LanePhase::Stopping)) {
    return;
  }
  attempt_.first_buffer_received = true;
  MaybeCompleteReadiness();
}

void SessionLane::HandleAudioError(const AudioErrorEvent& ev) {
  if (ev.token != attempt_.token ||
      (phase_ != LanePhase::Starting && phase_ != LanePhase::Recording &&
       phase_ != LanePhase::Stopping)) {
    return;
  }
  VLOG_ERROR("audio input stream error: {}", ev.error);
  FailActiveAttempt(ev.error, /*abort_capture=*/true);
}

void SessionLane::HandleSourceEof(const SourceEofEvent& ev) {
  if (ev.token != attempt_.token) {
    return;
  }
  attempt_.source_eof = true;
  const std::string scene_id =
      attempt_.pending_stop ? attempt_.pending_stop->scene_id : attempt_.default_scene_id;
  if (phase_ == LanePhase::Starting) {
    attempt_.pending_stop =
        PendingStop{.ime = attempt_.ime, .scene_id = scene_id, .stop_at = Clock::now()};
    attempt_.stop_at = attempt_.pending_stop->stop_at;
    phase_ = LanePhase::Stopping;
    MaybeCompleteReadiness();
    return;
  }
  if (phase_ == LanePhase::Stopping) {
    MaybeCompleteReadiness();
    return;
  }
  if (phase_ == LanePhase::Recording) {
    attempt_.pending_stop =
        PendingStop{.ime = attempt_.ime, .scene_id = scene_id, .stop_at = Clock::now()};
    attempt_.stop_at = attempt_.pending_stop->stop_at;
    phase_ = LanePhase::Stopping;
    FinishCaptureAndSubmitOrDiscard();
  }
}

void SessionLane::HandleAsrSessionReady(AsrSessionReadyEvent& ev) {
  if (ev.token != attempt_.token ||
      (phase_ != LanePhase::Starting && phase_ != LanePhase::Stopping)) {
    if (ev.session) {
      ev.session->Cancel();
      RetireSession(std::move(ev.session));
    }
    return;
  }

  if (!ev.ok || !ev.session) {
    std::string message = _("Failed to start recognition session.");
    if (!ev.error.empty()) {
      message += " " + ev.error;
    }
    FailActiveAttempt(message, /*abort_capture=*/true);
    return;
  }

  attempt_.asr_ready = true;
  attempt_.asr_ready_at = Clock::now();
  attempt_.asr_session = std::move(ev.session);
  attempt_.backend = ev.backend;
  MaybeCompleteReadiness();
}

void SessionLane::HandleRecognizedText(const RecognizedTextEvent& ev) {
  if (ev.token != attempt_.token || phase_ != LanePhase::Inferring) {
    return;
  }
  attempt_.latest_final_text = ev.text;
  if (deps_.event_sink != nullptr && !ev.text.empty()) {
    deps_.event_sink->OnPartialText(ev.text, attempt_.ime);
  }
}

void SessionLane::HandleEnterPostprocessing(const EnterPostprocessingEvent& ev) {
  if (ev.token != attempt_.token || phase_ != LanePhase::Inferring) {
    return;
  }
  phase_ = LanePhase::Postprocessing;
  if (deps_.event_sink != nullptr) {
    deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Postprocessing, attempt_.ime);
  }
}

void SessionLane::HandleWorkerCompleted(WorkerCompletedEvent& ev) {
  if (ev.token != attempt_.token ||
      (phase_ != LanePhase::Inferring && phase_ != LanePhase::Postprocessing)) {
    return;
  }

  if (!ev.recognition_ok) {
    const std::string failure = ev.recognition_error.empty()
                                    ? _("Failed to finish recognition session.")
                                    : ev.recognition_error;
    VLOG_ERROR("recognition error: {}", failure);
    if (deps_.event_sink != nullptr) {
      deps_.event_sink->OnNotification(
          vinput::daemon::notify::MakeDaemonErrorNotification(failure));
    }
    TransitionToIdle(true);
    return;
  }

  auto& result = ev.pipeline_result;
  switch (attempt_.postprocessing_action) {
  case PostprocessingAction::CommitRaw:
    result.payload.commitText = attempt_.latest_final_text;
    result.payload.candidates = {
        {.text = attempt_.latest_final_text, .source = vinput::result::kSourceRaw}};
    result.errors.clear();
    break;
  case PostprocessingAction::Discard:
    result.payload = {};
    result.errors.clear();
    break;
  case PostprocessingAction::None:
    break;
  }

  if (deps_.event_sink != nullptr) {
    for (const auto& error : result.errors) {
      if (!error.empty()) {
        VLOG_ERROR("processing error: {}", error);
        deps_.event_sink->OnNotification(
            vinput::daemon::notify::MakeDaemonErrorNotification(error));
      }
    }
    deps_.event_sink->OnRecognitionResult(result.payload, attempt_.ime);
  }

  TransitionToIdle(true);
}

void SessionLane::DrainIncomingAudio() {
  if (deps_.audio_source == nullptr) {
    return;
  }
  deps_.audio_source->DrainAvailable(
      [this](std::span<const int16_t> pcm) { HandleIncomingPcm(pcm); });
  if (phase_ == LanePhase::Recording && !UsesBufferedDelivery(attempt_.backend)) {
    FlushStreamingChunks();
  }
}

void SessionLane::WarnClippedHeadIfNeeded() {
  if (!attempt_.first_non_silent_at.has_value()) {
    attempt_.first_non_silent_at = Clock::now();
  }
  if (deps_.warn_on_clipped_head && !attempt_.clipped_head_warned) {
    attempt_.clipped_head_warned = true;
    VLOG_WARN("first audio buffer already contains speech-level energy; head may be clipped");
  }
}

void SessionLane::InspectFirstChunkSpeech(std::span<const int16_t> pcm) {
  if (pcm.empty()) {
    return;
  }
  const bool is_first_chunk = !attempt_.first_chunk_inspected;
  attempt_.first_chunk_inspected = true;
  if (!vinput::audio::HasNonSilentAudio(pcm, audio_buffer_.Gain())) {
    return;
  }
  if (is_first_chunk) {
    WarnClippedHeadIfNeeded();
    return;
  }
  if (!attempt_.first_non_silent_at.has_value()) {
    attempt_.first_non_silent_at = Clock::now();
  }
}

void SessionLane::HandleIncomingPcm(std::span<const int16_t> pcm) {
  if (pcm.empty()) {
    return;
  }
  if (phase_ == LanePhase::Starting || phase_ == LanePhase::Stopping) {
    InspectFirstChunkSpeech(pcm);
    audio_buffer_.AppendStartup(pcm);
    return;
  }
  if (phase_ != LanePhase::Recording) {
    return;
  }

  const bool is_first_chunk = !attempt_.first_chunk_inspected;
  attempt_.first_chunk_inspected = true;
  const bool has_speech = audio_buffer_.AppendAndDetectSpeech(pcm);
  if (has_speech) {
    if (is_first_chunk) {
      WarnClippedHeadIfNeeded();
    } else if (!attempt_.first_non_silent_at.has_value()) {
      attempt_.first_non_silent_at = Clock::now();
    }
  }
}

void SessionLane::FlushStreamingChunks() {
  if (!attempt_.asr_session) {
    return;
  }
  while (auto chunk = audio_buffer_.PopStreamingChunk()) {
    std::string push_error;
    if (!attempt_.asr_session->PushAudio(*chunk, &push_error) || !push_error.empty()) {
      FailActiveAttempt(push_error.empty() ? _("Failed to push audio chunk.") : push_error,
                        /*abort_capture=*/true);
      return;
    }
  }
}

bool SessionLane::ApplySingleRecognitionEvent(const vinput::daemon::asr::RecognitionEvent& event) {
  switch (event.kind) {
  case vinput::daemon::asr::RecognitionEventKind::PartialText:
    if (!event.text.empty() && deps_.event_sink != nullptr) {
      if (!attempt_.first_partial_logged) {
        attempt_.first_partial_logged = true;
        VLOG_DEBUG("first partial after {} ms", DeltaMs(attempt_.start_received_at, Clock::now()));
      }
      deps_.event_sink->OnPartialText(event.text, attempt_.ime);
    }
    return true;
  case vinput::daemon::asr::RecognitionEventKind::FinalText:
    if (!event.text.empty()) {
      attempt_.latest_final_text = event.text;
      if (deps_.event_sink != nullptr) {
        deps_.event_sink->OnPartialText(event.text, attempt_.ime);
      }
    }
    return true;
  case vinput::daemon::asr::RecognitionEventKind::Error:
    FailActiveAttempt(event.error.empty() ? _("ASR provider failed.") : event.error,
                      /*abort_capture=*/true);
    return false;
  case vinput::daemon::asr::RecognitionEventKind::Completed:
    return true;
  }
  return true;
}

void SessionLane::ApplyRecognitionEvents(
    const std::vector<vinput::daemon::asr::RecognitionEvent>& events) {
  for (const auto& event : events) {
    if (!ApplySingleRecognitionEvent(event)) {
      return;
    }
  }
}

void SessionLane::PollActiveSessionEvents() {
  if (phase_ != LanePhase::Recording || !attempt_.asr_session ||
      UsesBufferedDelivery(attempt_.backend)) {
    return;
  }
  const auto events = attempt_.asr_session->PollEvents();
  if (!events.empty()) {
    ApplyRecognitionEvents(events);
  }
}

void SessionLane::CheckReadinessTimeout() {
  if ((phase_ != LanePhase::Starting && phase_ != LanePhase::Stopping) ||
      attempt_.first_buffer_received || attempt_.first_buffer_timed_out || attempt_.source_eof ||
      !attempt_.connect_returned_at.has_value()) {
    return;
  }
  const auto elapsed = Clock::now() - *attempt_.connect_returned_at;
  if (elapsed < deps_.first_buffer_ready_timeout) {
    return;
  }
  attempt_.first_buffer_timed_out = true;
  VLOG_WARN("first audio buffer readiness timed out after {} ms; proceeding",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
  MaybeCompleteReadiness();
}

void SessionLane::MaybeCompleteReadiness() {
  if (phase_ != LanePhase::Starting && phase_ != LanePhase::Stopping) {
    return;
  }
  const bool capture_ready =
      attempt_.first_buffer_received || attempt_.first_buffer_timed_out || attempt_.source_eof;
  if (!capture_ready || !attempt_.asr_ready) {
    return;
  }

  audio_buffer_.Activate(UsesBufferedDelivery(attempt_.backend));

  if (attempt_.pending_stop.has_value()) {
    phase_ = LanePhase::Stopping;
    FinishCaptureAndSubmitOrDiscard();
    return;
  }

  phase_ = LanePhase::Recording;
  attempt_.recording_published_at = Clock::now();
  if (deps_.event_sink != nullptr) {
    deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Recording, attempt_.ime);
  }
  if (!UsesBufferedDelivery(attempt_.backend)) {
    FlushStreamingChunks();
  }
}

void SessionLane::LogHeadGapMetrics(std::size_t total_samples, std::size_t dropped_samples) const {
  const long connect_ms = DeltaMs(attempt_.start_received_at, attempt_.connect_returned_at);
  const long first_buffer_ms = DeltaMs(attempt_.start_received_at, attempt_.first_buffer_at);
  const long first_signal_ms = DeltaMs(attempt_.start_received_at, attempt_.first_non_silent_at);
  const long asr_ready_ms = DeltaMs(attempt_.start_received_at, attempt_.asr_ready_at);
  const long recording_ms = DeltaMs(attempt_.start_received_at, attempt_.recording_published_at);
  const long stop_ms = DeltaMs(attempt_.start_received_at, attempt_.stop_at);

  VLOG_INFO("recording timing connect_ms={} first_buffer_ms={} first_signal_ms={} asr_ready_ms={} "
            "recording_ms={} stop_ms={} samples={} dropped_samples={} clipped_head={}",
            connect_ms, first_buffer_ms, first_signal_ms, asr_ready_ms, recording_ms, stop_ms,
            total_samples, dropped_samples, attempt_.clipped_head_warned ? 1 : 0);
}

std::size_t SessionLane::DrainTailOnStop() {
  if (deps_.audio_source == nullptr) {
    return 0;
  }
  const std::size_t dropped_samples = deps_.audio_source->DroppedSamples();
  if (dropped_samples > 0) {
    VLOG_WARN("capture audio queue dropped {} samples due to overflow", dropped_samples);
  }
  const auto tail = deps_.audio_source->StopAndDrain();
  if (tail.empty()) {
    return dropped_samples;
  }
  if (!attempt_.first_chunk_inspected) {
    InspectFirstChunkSpeech(tail);
  }
  if (audio_buffer_.AppendAndDetectSpeech(tail) && !attempt_.first_non_silent_at.has_value()) {
    attempt_.first_non_silent_at = Clock::now();
  }
  return dropped_samples;
}

void SessionLane::FinishCaptureAndSubmitOrDiscard() {
  const std::size_t dropped_samples = DrainTailOnStop();
  RestoreDuckerIfNeeded();

  // A terminal audio error may have been posted while draining the source; fail the
  // attempt instead of submitting a recognition over silently degraded capture.
  if (const auto pending_error = PendingTerminalAudioError()) {
    VLOG_ERROR("audio input failed while stopping, skipping inference: {}", *pending_error);
    FailActiveAttempt(*pending_error, /*abort_capture=*/true);
    return;
  }

  const std::size_t total_samples = audio_buffer_.TotalSampleCount();
  LogHeadGapMetrics(total_samples, dropped_samples);

  if (total_samples < vinput::daemon::asr::kMinSamplesForRecognition) {
    VLOG_INFO("recording too short, skipping inference: {} samples ({:.1f} ms)", total_samples,
              static_cast<double>(total_samples) * 1000.0 / 16000.0);
    // Stream clients only see the status line; report the drop explicitly so they can
    // distinguish it from an empty recognition result.
    if (!attempt_.ime && deps_.event_sink != nullptr) {
      deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Error, /*ime=*/false);
    }
    RetireSession(std::move(attempt_.asr_session));
    TransitionToIdle(true);
    return;
  }

  SubmitInferenceOrder(total_samples);
}

void SessionLane::SubmitInferenceOrder(std::size_t total_samples) {
  const bool buffered = UsesBufferedDelivery(attempt_.backend);
  std::vector<int16_t> pcm =
      buffered ? audio_buffer_.TakeBufferedPcm() : audio_buffer_.DrainStreamingTail();
  const std::string scene_id =
      attempt_.pending_stop.has_value() ? attempt_.pending_stop->scene_id : std::string{};

  VLOG_DEBUG("ASR request provider={} type={} backend={} samples={}", attempt_.backend.provider_id,
             attempt_.backend.provider_type, attempt_.backend.backend_id, total_samples);

  attempt_.cancel_flag = std::make_shared<std::atomic<bool>>(false);
  phase_ = LanePhase::Inferring;
  if (deps_.event_sink != nullptr) {
    deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Inferring, attempt_.ime);
  }

  EnsureWorkerStarted();
  SubmitWorkerTask(ProcessOrderTask{
      .token = attempt_.token,
      .audio_delivery_mode = attempt_.backend.capabilities.audio_delivery_mode,
      .session = std::move(attempt_.asr_session),
      .backend = attempt_.backend,
      .pcm = std::move(pcm),
      .postprocess_request =
          PostprocessRequest{
              .text = attempt_.latest_final_text,
              .scene_id = scene_id,
              .is_command = attempt_.is_command,
              .selected_text = attempt_.selected_text,
          },
      .settings = attempt_.settings,
      .ime = attempt_.ime,
      .cancel_flag = attempt_.cancel_flag,
  });
}

void SessionLane::TransitionToIdle(bool emit_status) {
  const bool was_ime = attempt_.ime;
  audio_buffer_.Clear();
  attempt_ = ActiveAttempt{};
  phase_ = LanePhase::Idle;
  if (emit_status && deps_.event_sink != nullptr) {
    deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Idle, was_ime);
  }
}

void SessionLane::FailActiveAttempt(const std::string& error_message, bool abort_capture) {
  if (abort_capture && deps_.audio_source != nullptr) {
    deps_.audio_source->Abort();
  }
  RestoreDuckerIfNeeded();
  RetireSession(std::move(attempt_.asr_session));

  const bool was_ime = attempt_.ime;
  if (deps_.event_sink != nullptr) {
    if (!error_message.empty()) {
      deps_.event_sink->OnNotification(
          vinput::daemon::notify::MakeDaemonErrorNotification(error_message));
    }
    deps_.event_sink->OnStatusChanged(vinput::dbus::Status::Error, was_ime);
  }
  TransitionToIdle(true);
}

void SessionLane::RestoreDuckerIfNeeded() {
  if (attempt_.ducked && deps_.output_ducker != nullptr) {
    attempt_.ducked = false;
    deps_.output_ducker->Restore();
  }
}

void SessionLane::RetireSession(std::shared_ptr<vinput::daemon::asr::RecognitionSession> session) {
  if (!session) {
    return;
  }
  session->Cancel();
  retired_sessions_.push_back(std::move(session));
}

void SessionLane::FlushRetiredSessions() {
  retired_sessions_.clear();
}

void SessionLane::EnsureWorkerStarted() {
  const std::scoped_lock lock(worker_mutex_);
  if (worker_running_ || shutting_down_) {
    return;
  }
  worker_running_ = true;
  worker_ = std::thread([this] { WorkerMain(); });
}

void SessionLane::SubmitWorkerTask(WorkerTask task) {
  {
    const std::scoped_lock lock(worker_mutex_);
    pending_task_ = std::move(task);
  }
  worker_cv_.notify_one();
}

void SessionLane::WorkerMain() {
  while (true) {
    std::optional<WorkerTask> task;
    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_cv_.wait(lock, [this] { return pending_task_.has_value() || shutting_down_; });
      if (shutting_down_ && !pending_task_.has_value()) {
        worker_running_ = false;
        return;
      }
      task = std::move(pending_task_);
      pending_task_.reset();
      if (task.has_value()) {
        if (auto* order_task = std::get_if<ProcessOrderTask>(&*task)) {
          // Register shutdown handles inside the same locked interval that claims the task;
          // otherwise Shutdown could miss both the pending task and the in-flight session.
          in_flight_session_ = order_task->session;
          in_flight_cancel_flag_ = order_task->cancel_flag;
        }
      }
    }

    if (!task.has_value()) {
      continue;
    }
    if (auto* create_task = std::get_if<CreateAsrSessionTask>(&*task)) {
      ExecuteCreateAsrSession(*create_task);
    } else if (auto* order_task = std::get_if<ProcessOrderTask>(&*task)) {
      ExecuteProcessOrder(std::move(*order_task));
    }
  }
}

void SessionLane::ExecuteCreateAsrSession(const CreateAsrSessionTask& task) {
  vinput::daemon::asr::BackendDescriptor descriptor;
  std::string error;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  if (deps_.create_asr_session) {
    try {
      session = deps_.create_asr_session(task.settings, &descriptor, &error,
                                         [this] { PostInboxEvent(WakeOnlyEvent{}); });
    } catch (const std::exception& e) {
      session.reset();
      error = e.what();
    } catch (...) {
      session.reset();
      error = _("Unknown error during processing.");
    }
  } else {
    error = _("ASR session factory is not configured.");
  }

  {
    const std::scoped_lock lock(worker_mutex_);
    if (shutting_down_) {
      if (session) {
        session->Cancel();
      }
      return;
    }
  }

  PostInboxEvent(AsrSessionReadyEvent{
      .token = task.token,
      .ok = (session != nullptr),
      .session = std::move(session),
      .backend = std::move(descriptor),
      .error = std::move(error),
  });
}

bool SessionLane::RunRecognitionOnWorker(ProcessOrderTask& task, std::string* error_out) {
  if (!task.session) {
    return true;
  }

  if (!PushOrderPcm(*task.session, task.pcm, task.audio_delivery_mode,
                    task.settings.asr.normalizeAudio, error_out)) {
    return false;
  }

  std::string finish_error;
  bool ok = task.session->Finish(&finish_error);
  const auto events = task.session->PollEvents();
  std::string final_text;
  CollectFinishEvents(events, final_text, finish_error, ok);
  if (!ok || !finish_error.empty()) {
    *error_out =
        finish_error.empty() ? _("Failed to finish recognition session.") : std::move(finish_error);
    return false;
  }

  if (!final_text.empty()) {
    task.postprocess_request.text = std::move(final_text);
    PostInboxEvent(RecognizedTextEvent{.token = task.token, .text = task.postprocess_request.text});
  }
  return true;
}

void SessionLane::ExecuteProcessOrder(ProcessOrderTask task) {
  std::string recognition_error;
  bool recognition_ok = false;
  RecognitionPipelineResult pipeline_result;
  try {
    recognition_ok = RunRecognitionOnWorker(task, &recognition_error);
    task.session.reset();
    if (recognition_ok && !task.postprocess_request.text.empty() && deps_.run_postprocess) {
      pipeline_result = deps_.run_postprocess(
          task.postprocess_request, task.settings,
          [this, token = task.token] { PostInboxEvent(EnterPostprocessingEvent{.token = token}); },
          task.cancel_flag.get());
    }
  } catch (const std::exception& e) {
    VLOG_ERROR("worker exception: {}", e.what());
    recognition_ok = false;
    recognition_error = e.what();
  } catch (...) {
    VLOG_ERROR("worker unknown exception");
    recognition_ok = false;
    recognition_error = _("Unknown error during processing.");
  }
  task.session.reset();

  {
    const std::scoped_lock lock(worker_mutex_);
    in_flight_session_.reset();
    in_flight_cancel_flag_.reset();
  }

  PostInboxEvent(WorkerCompletedEvent{
      .token = task.token,
      .recognition_ok = recognition_ok,
      .recognition_error = std::move(recognition_error),
      .pipeline_result = std::move(pipeline_result),
  });
}

void SessionLane::Shutdown() {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> in_flight_session;
  std::shared_ptr<std::atomic<bool>> in_flight_cancel;
  {
    const std::scoped_lock lock(worker_mutex_);
    shutting_down_ = true;
    pending_task_.reset();
    in_flight_session = in_flight_session_;
    in_flight_cancel = in_flight_cancel_flag_;
  }

  if (deps_.audio_source != nullptr) {
    deps_.audio_source->SetNotifyFd(-1);
    deps_.audio_source->Abort();
  }
  RestoreDuckerIfNeeded();
  if (attempt_.asr_session) {
    attempt_.asr_session->Cancel();
    attempt_.asr_session.reset();
  }
  if (in_flight_cancel) {
    in_flight_cancel->store(true, std::memory_order_release);
  }
  if (in_flight_session) {
    in_flight_session->Interrupt();
  }

  worker_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }

  {
    const std::scoped_lock lock(inbox_mutex_);
    inbox_.clear();
  }
  retired_sessions_.clear();
  audio_buffer_.Clear();
  attempt_ = ActiveAttempt{};
  phase_ = LanePhase::Idle;
}

} // namespace vinput::daemon::runtime
