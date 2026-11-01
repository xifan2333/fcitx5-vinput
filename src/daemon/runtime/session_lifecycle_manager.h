#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/runtime/recognition_session_manager.h"

namespace vinput::daemon::runtime {

struct SessionPollResult {
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  std::vector<vinput::daemon::asr::RecognitionEvent> events;
  std::optional<std::string> failure;
  bool session_active = false;
};

class SessionLifecycleManager {
public:
  using WakeCallback = std::function<void()>;

  SessionLifecycleManager() = default;
  ~SessionLifecycleManager();

  SessionLifecycleManager(const SessionLifecycleManager&) = delete;
  SessionLifecycleManager& operator=(const SessionLifecycleManager&) = delete;
  SessionLifecycleManager(SessionLifecycleManager&&) = delete;
  SessionLifecycleManager& operator=(SessionLifecycleManager&&) = delete;

  void SetWakeCallback(WakeCallback callback);

  void InstallActiveSession(std::shared_ptr<vinput::daemon::asr::RecognitionSession> session);

  [[nodiscard]] bool HasActiveSession() const;
  [[nodiscard]] std::shared_ptr<vinput::daemon::asr::RecognitionSession> ActiveSession() const;
  [[nodiscard]] bool
  IsActiveSession(const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session) const;

  std::shared_ptr<vinput::daemon::asr::RecognitionSession> DetachActive();

  bool PushAudio(const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
                 std::span<const int16_t> pcm, std::string* error = nullptr);
  bool PushAudio(const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
                 std::span<const float> samples, std::string* error = nullptr);

  [[nodiscard]] SessionPollResult PollActiveEvents();

  [[nodiscard]] vinput::daemon::asr::RecognitionRunResult FinishSessionAndCollectResult(
      const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
      std::string* error = nullptr);

  void CancelAndRetireActive();

  bool CancelAndRetireActiveIfMatches(
      const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& expected_session);

  void CancelAndRetireSession(std::shared_ptr<vinput::daemon::asr::RecognitionSession> session);

  void RetireSession(std::shared_ptr<vinput::daemon::asr::RecognitionSession> session);

  std::size_t FlushReapedSessions();

  [[nodiscard]] std::size_t DeferredSessionCount() const;

  void Clear();

private:
  template <typename SampleSpan>
  bool PushAudioInternal(const std::shared_ptr<vinput::daemon::asr::RecognitionSession>& session,
                         SampleSpan samples, std::string* error);

  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex session_io_mutex_;
  WakeCallback wake_callback_;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> active_session_;
  std::vector<std::shared_ptr<vinput::daemon::asr::RecognitionSession>>
      deferred_cancelled_sessions_;
};

} // namespace vinput::daemon::runtime
