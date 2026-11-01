#include "daemon/runtime/session_lifecycle_manager.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/i18n.h"

#include "daemon/asr/runtime/async_recognition_session.h"
#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/runtime/recognition_session_manager.h"

namespace vinput::daemon::runtime {

namespace {

constexpr const char* kSessionNotInitializedError = "Recognition session is not initialized.";
constexpr const char* kFailedToPushAudioError = "Failed to push audio chunk.";
constexpr const char* kFailedToFinishSessionError = "Failed to finish recognition session.";

template <typename Fn>
bool PushAudioWithLock(std::mutex& io_mutex,
                       const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
                       Fn&& push_fn, std::string* error) {
  if (!session) {
    if (error != nullptr) {
      *error = kSessionNotInitializedError;
    }
    return false;
  }
  const std::scoped_lock lock(io_mutex);
  const bool ok = std::forward<Fn>(push_fn)();
  if (!ok && error != nullptr && error->empty()) {
    *error = kFailedToPushAudioError;
  }
  return ok;
}

} // namespace

SessionLifecycleManager::~SessionLifecycleManager() {
  Clear();
}

void SessionLifecycleManager::SetWakeCallback(WakeCallback callback) {
  const std::scoped_lock lock(lifecycle_mutex_);
  wake_callback_ = std::move(callback);
}

void SessionLifecycleManager::InstallActiveSession(
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> session) {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> old_session;
  {
    const std::scoped_lock lock(lifecycle_mutex_);
    old_session = std::move(active_session_);
    active_session_ = std::move(session);
  }
  if (old_session) {
    CancelAndRetireSession(std::move(old_session));
  }
}

bool SessionLifecycleManager::HasActiveSession() const {
  const std::scoped_lock lock(lifecycle_mutex_);
  return active_session_ != nullptr;
}

std::shared_ptr<vinput::daemon::asr::RecognitionSession>
SessionLifecycleManager::ActiveSession() const {
  const std::scoped_lock lock(lifecycle_mutex_);
  return active_session_;
}

bool SessionLifecycleManager::IsActiveSession(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session) const {
  const std::scoped_lock lock(lifecycle_mutex_);
  return session != nullptr && active_session_ == session;
}

std::shared_ptr<vinput::daemon::asr::RecognitionSession> SessionLifecycleManager::DetachActive() {
  const std::scoped_lock lock(lifecycle_mutex_);
  return std::move(active_session_);
}

bool SessionLifecycleManager::PushAudio(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
    std::span<const int16_t> pcm, std::string* error) {
  return PushAudioWithLock(
      session_io_mutex_, session, [&] { return session->PushAudio(pcm, error); }, error);
}

bool SessionLifecycleManager::PushAudio(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
    std::span<const float> samples, std::string* error) {
  return PushAudioWithLock(
      session_io_mutex_, session, [&] { return session->PushAudio(samples, error); }, error);
}

SessionPollResult SessionLifecycleManager::PollSessionEvents(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session) const {
  if (!session) {
    return SessionPollResult{
        .session = nullptr,
        .events = {},
        .failure = std::nullopt,
        .session_active = false,
    };
  }

  std::vector<vinput::daemon::asr::RecognitionEvent> events;
  std::optional<std::string> failure;
  {
    const std::scoped_lock session_lock(session_io_mutex_);
    events = session->PollEvents();
    if (auto* async_session =
            dynamic_cast<vinput::daemon::asr::AsyncRecognitionSession*>(session.get())) {
      failure = async_session->FailureError();
    }
  }

  if (!failure.has_value()) {
    for (const auto& event : events) {
      if (event.kind == vinput::daemon::asr::RecognitionEventKind::Error) {
        failure = event.error.empty() ? _("ASR provider failed.") : event.error;
        break;
      }
    }
  }

  if (failure.has_value()) {
    std::erase_if(events, [](const vinput::daemon::asr::RecognitionEvent& event) {
      return event.kind == vinput::daemon::asr::RecognitionEventKind::Error;
    });
  }

  const bool still_active = IsActiveSession(session);

  return SessionPollResult{
      .session = session,
      .events = std::move(events),
      .failure = std::move(failure),
      .session_active = still_active,
  };
}

SessionPollResult SessionLifecycleManager::PollActiveEvents() const {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  {
    const std::scoped_lock lock(lifecycle_mutex_);
    session = active_session_;
  }
  return PollSessionEvents(session);
}

vinput::daemon::asr::RecognitionRunResult SessionLifecycleManager::FinishSessionAndCollectResult(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session, std::string* error) {
  vinput::daemon::asr::RecognitionRunResult result;
  if (!session) {
    if (error != nullptr) {
      *error = kSessionNotInitializedError;
    }
    result.available = false;
    result.ok = false;
    result.error = (error != nullptr) ? *error : kSessionNotInitializedError;
    return result;
  }

  result.available = true;
  std::string session_error;
  std::vector<vinput::daemon::asr::RecognitionEvent> events;
  {
    const std::scoped_lock session_lock(session_io_mutex_);
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

  if (error != nullptr) {
    *error = result.error;
  }
  return result;
}

void SessionLifecycleManager::CancelAndRetireSession(
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> session) {
  if (!session) {
    return;
  }
  {
    const std::scoped_lock io_lock(session_io_mutex_);
    session->Cancel();
  }
  RetireSession(std::move(session));
}

void SessionLifecycleManager::CancelAndRetireActive() {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;
  {
    const std::scoped_lock lock(lifecycle_mutex_);
    session_to_cancel = std::move(active_session_);
  }
  if (!session_to_cancel) {
    return;
  }
  CancelAndRetireSession(std::move(session_to_cancel));
}

bool SessionLifecycleManager::CancelAndRetireActiveIfMatches(
    const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& expected_session) {
  if (!expected_session) {
    return false;
  }
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session_to_cancel;
  {
    const std::scoped_lock lock(lifecycle_mutex_);
    if (active_session_ != expected_session) {
      return false;
    }
    session_to_cancel = std::move(active_session_);
  }
  CancelAndRetireSession(std::move(session_to_cancel));
  return true;
}

void SessionLifecycleManager::RetireSession(
    std::shared_ptr<vinput::daemon::asr::RecognitionSession> session) {
  if (!session) {
    return;
  }
  WakeCallback wake;
  {
    const std::scoped_lock lock(lifecycle_mutex_);
    deferred_cancelled_sessions_.push_back(std::move(session));
    wake = wake_callback_;
  }
  if (wake) {
    wake();
  }
}

std::size_t SessionLifecycleManager::FlushReapedSessions() {
  std::vector<std::shared_ptr<vinput::daemon::asr::RecognitionSession>> deferred;
  {
    const std::scoped_lock lock(lifecycle_mutex_);
    deferred.swap(deferred_cancelled_sessions_);
  }
  const std::size_t count = deferred.size();
  deferred.clear();
  return count;
}

std::size_t SessionLifecycleManager::DeferredSessionCount() const {
  const std::scoped_lock lock(lifecycle_mutex_);
  return deferred_cancelled_sessions_.size();
}

void SessionLifecycleManager::Clear() {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> active;
  std::vector<std::shared_ptr<vinput::daemon::asr::RecognitionSession>> deferred;
  {
    const std::scoped_lock lock(lifecycle_mutex_);
    active = std::move(active_session_);
    deferred.swap(deferred_cancelled_sessions_);
  }
  if (active) {
    const std::scoped_lock io_lock(session_io_mutex_);
    active->Cancel();
  }
  active.reset();
  deferred.clear();
}

} // namespace vinput::daemon::runtime
