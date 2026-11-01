#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "daemon/asr/runtime/async_recognition_session.h"
#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/runtime/session_lifecycle_manager.h"

namespace {

using vinput::daemon::asr::AsyncRecognitionSession;
using vinput::daemon::asr::RecognitionEvent;
using vinput::daemon::asr::RecognitionEventKind;
using vinput::daemon::asr::RecognitionSession;
using vinput::daemon::runtime::SessionLifecycleManager;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

class MockSession final : public RecognitionSession {
public:
  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    const std::scoped_lock lock(mutex_);
    ++push_calls_;
    if (fail_push_) {
      if (error != nullptr) {
        *error = "mock push failed";
      }
      return false;
    }
    pcm_received_.insert(pcm_received_.end(), pcm.begin(), pcm.end());
    return true;
  }

  bool PushAudio(std::span<const float> samples, std::string* error) override {
    const std::scoped_lock lock(mutex_);
    ++push_calls_;
    if (fail_push_) {
      if (error != nullptr) {
        *error = "mock push float failed";
      }
      return false;
    }
    float_received_.insert(float_received_.end(), samples.begin(), samples.end());
    return true;
  }

  bool Finish(std::string* error) override {
    const std::scoped_lock lock(mutex_);
    ++finish_calls_;
    if (fail_finish_) {
      if (error != nullptr) {
        *error = "mock finish failed";
      }
      return false;
    }
    events_.push_back({
        .kind = RecognitionEventKind::FinalText,
        .text = finish_text_,
        .error = {},
    });
    return true;
  }

  void Cancel() override {
    const std::scoped_lock lock(mutex_);
    ++cancel_calls_;
    cancelled_ = true;
  }

  std::vector<RecognitionEvent> PollEvents() override {
    const std::scoped_lock lock(mutex_);
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

  void SetFailPush(bool fail) {
    const std::scoped_lock lock(mutex_);
    fail_push_ = fail;
  }

  void SetFailFinish(bool fail) {
    const std::scoped_lock lock(mutex_);
    fail_finish_ = fail;
  }

  void SetFinishText(std::string text) {
    const std::scoped_lock lock(mutex_);
    finish_text_ = std::move(text);
  }

  void QueueEvent(RecognitionEvent event) {
    const std::scoped_lock lock(mutex_);
    events_.push_back(std::move(event));
  }

  [[nodiscard]] int PushCalls() const {
    const std::scoped_lock lock(mutex_);
    return push_calls_;
  }

  [[nodiscard]] int CancelCalls() const {
    const std::scoped_lock lock(mutex_);
    return cancel_calls_;
  }

  [[nodiscard]] int FinishCalls() const {
    const std::scoped_lock lock(mutex_);
    return finish_calls_;
  }

  [[nodiscard]] bool IsCancelled() const {
    const std::scoped_lock lock(mutex_);
    return cancelled_;
  }

  [[nodiscard]] std::size_t PcmReceivedCount() const {
    const std::scoped_lock lock(mutex_);
    return pcm_received_.size();
  }

  [[nodiscard]] std::size_t FloatReceivedCount() const {
    const std::scoped_lock lock(mutex_);
    return float_received_.size();
  }

private:
  mutable std::mutex mutex_;
  std::vector<int16_t> pcm_received_;
  std::vector<float> float_received_;
  std::vector<RecognitionEvent> events_;
  std::string finish_text_ = "hello world";
  int push_calls_ = 0;
  int finish_calls_ = 0;
  int cancel_calls_ = 0;
  bool fail_push_ = false;
  bool fail_finish_ = false;
  bool cancelled_ = false;
};

void TestInstallAndQuery() {
  SessionLifecycleManager manager;
  Expect(!manager.HasActiveSession(), "Must not have active session initially");
  Expect(manager.ActiveSession() == nullptr, "ActiveSession() must be null initially");
  Expect(manager.DeferredSessionCount() == 0, "DeferredSessionCount must be 0 initially");

  auto session1 = std::make_shared<MockSession>();
  manager.InstallActiveSession(session1);
  Expect(manager.HasActiveSession(), "Must have active session after install");
  Expect(manager.ActiveSession() == session1, "ActiveSession must match installed session");
  Expect(manager.IsActiveSession(session1), "IsActiveSession must be true for session1");
  Expect(!session1->IsCancelled(), "session1 must not be cancelled on install");

  // Installing session2 replaces session1, cancelling session1 and retiring it to deferred queue
  auto session2 = std::make_shared<MockSession>();
  manager.InstallActiveSession(session2);
  Expect(manager.ActiveSession() == session2, "ActiveSession must now be session2");
  Expect(manager.IsActiveSession(session2), "IsActiveSession must be true for session2");
  Expect(!manager.IsActiveSession(session1), "IsActiveSession must be false for session1");
  Expect(session1->IsCancelled(), "session1 must be cancelled when superseded");
  Expect(manager.DeferredSessionCount() == 1, "session1 must be in deferred queue");

  // Flushing reaps session1
  const auto reaped = manager.FlushReapedSessions();
  Expect(reaped == 1, "Must have reaped exactly 1 session");
  Expect(manager.DeferredSessionCount() == 0, "Deferred count must be 0 after flush");
}

void TestDetachActive() {
  SessionLifecycleManager manager;
  auto session = std::make_shared<MockSession>();
  manager.InstallActiveSession(session);

  auto detached = manager.DetachActive();
  Expect(detached == session, "Detached session must match installed session");
  Expect(!manager.HasActiveSession(), "Must not have active session after detach");
  Expect(manager.ActiveSession() == nullptr, "ActiveSession must be null after detach");
  Expect(!session->IsCancelled(), "Detaching session must NOT cancel it");
  Expect(manager.DeferredSessionCount() == 0, "Detached session must not be retired");

  // Detaching again returns nullptr
  Expect(manager.DetachActive() == nullptr, "Second detach must return nullptr");
}

void TestAudioPushing() {
  SessionLifecycleManager manager;
  auto session = std::make_shared<MockSession>();
  manager.InstallActiveSession(session);

  // Push PCM
  const std::vector<int16_t> pcm{100, 200, -300};
  std::string error;
  const bool ok_pcm = manager.PushAudio(session, pcm, &error);
  Expect(ok_pcm, "PushAudio PCM should succeed");
  Expect(session->PcmReceivedCount() == 3, "Session received 3 PCM samples");
  Expect(error.empty(), "Error string should remain empty on success");

  // Push float
  const std::vector<float> floats{0.1F, -0.2F};
  const bool ok_float = manager.PushAudio(session, floats, &error);
  Expect(ok_float, "PushAudio float should succeed");
  Expect(session->FloatReceivedCount() == 2, "Session received 2 float samples");

  // Null session
  const bool ok_null = manager.PushAudio(nullptr, pcm, &error);
  Expect(!ok_null, "PushAudio on null session must fail");
  Expect(!error.empty(), "PushAudio on null session must set error");

  // Backend error propagation
  session->SetFailPush(true);
  error.clear();
  const bool ok_fail = manager.PushAudio(session, pcm, &error);
  Expect(!ok_fail, "PushAudio must fail when backend fails");
  Expect(error == "mock push failed", "Error message should be propagated from backend");
}

void TestPollActiveEvents() {
  SessionLifecycleManager manager;
  auto session = std::make_shared<MockSession>();
  manager.InstallActiveSession(session);

  session->QueueEvent({.kind = RecognitionEventKind::PartialText, .text = "part1", .error = {}});
  session->QueueEvent({.kind = RecognitionEventKind::FinalText, .text = "final1", .error = {}});

  auto poll = manager.PollActiveEvents();
  Expect(poll.session_active, "Session must be active");
  Expect(poll.session == session, "Polled session must match");
  Expect(!poll.failure.has_value(), "No failure expected");
  Expect(poll.events.size() == 2, "Should have received 2 events");

  // Error event extraction
  session->QueueEvent({.kind = RecognitionEventKind::Error, .text = {}, .error = "backend died"});
  session->QueueEvent({.kind = RecognitionEventKind::PartialText, .text = "tail", .error = {}});

  poll = manager.PollActiveEvents();
  Expect(poll.session_active, "Session must still be active");
  Expect(poll.failure.has_value(), "Failure must be set from Error event");
  Expect(*poll.failure == "backend died", "Failure message must match");
  Expect(poll.events.size() == 1, "Error event should be filtered from event list");
  Expect(poll.events[0].text == "tail", "Non-error event preserved");

  // Poll when no active session
  manager.DetachActive();
  poll = manager.PollActiveEvents();
  Expect(!poll.session_active, "Session must not be active");
  Expect(poll.session == nullptr, "Session pointer must be null");
  Expect(poll.events.empty(), "Events must be empty");
}

void TestFinishSessionAndCollectResult() {
  SessionLifecycleManager manager;
  auto session = std::make_shared<MockSession>();
  session->SetFinishText("recognized transcript");

  std::string error;
  auto result = manager.FinishSessionAndCollectResult(session, &error);
  Expect(result.available, "Result must be available");
  Expect(result.ok, "Result must be ok");
  Expect(result.text == "recognized transcript", "Result text must match");
  Expect(error.empty(), "Error must be empty on success");
  Expect(session->FinishCalls() == 1, "Finish must have been called once");

  // Backend finish failure
  session->SetFailFinish(true);
  result = manager.FinishSessionAndCollectResult(session, &error);
  Expect(result.available, "Result must be available");
  Expect(!result.ok, "Result ok must be false on failure");
  Expect(error == "mock finish failed", "Error must match backend message");

  // Null session
  result = manager.FinishSessionAndCollectResult(nullptr, &error);
  Expect(!result.available, "Null session result must not be available");
  Expect(!result.ok, "Null session result ok must be false");
  Expect(!error.empty(), "Null session must set error");
}

void TestCancelAndRetirement() {
  SessionLifecycleManager manager;
  std::atomic<int> wake_count{0};
  manager.SetWakeCallback([&wake_count] { ++wake_count; });

  auto session = std::make_shared<MockSession>();
  manager.InstallActiveSession(session);

  manager.CancelAndRetireActive();
  Expect(!manager.HasActiveSession(), "Active session must be cleared after CancelAndRetireActive");
  Expect(session->IsCancelled(), "Session must be cancelled");
  Expect(manager.DeferredSessionCount() == 1, "Session must be in deferred retirement queue");
  Expect(wake_count == 1, "Wake callback must have been invoked once");

  // Arbitrary session retirement
  auto session_extra = std::make_shared<MockSession>();
  manager.RetireSession(session_extra);
  Expect(manager.DeferredSessionCount() == 2, "Deferred count must now be 2");
  Expect(wake_count == 2, "Wake callback must be invoked on RetireSession");

  const auto reaped = manager.FlushReapedSessions();
  Expect(reaped == 2, "Must have reaped 2 sessions");
  Expect(manager.DeferredSessionCount() == 0, "Deferred queue must be empty");
}

void TestCancelAndRetireActiveIfMatches() {
  SessionLifecycleManager manager;
  std::atomic<int> wake_count{0};
  manager.SetWakeCallback([&wake_count] { ++wake_count; });

  auto session_a = std::make_shared<MockSession>();
  auto session_b = std::make_shared<MockSession>();

  manager.InstallActiveSession(session_a);

  // Mismatched session: must not cancel or retire
  const bool cancelled_b = manager.CancelAndRetireActiveIfMatches(session_b);
  Expect(!cancelled_b, "Mismatched session must return false");
  Expect(manager.HasActiveSession(), "session_a must still be active");
  Expect(!session_a->IsCancelled(), "session_a must not be cancelled");
  Expect(manager.DeferredSessionCount() == 0, "No sessions should be deferred");
  Expect(wake_count == 0, "Wake callback must not be invoked");

  // Matching session: cancels and retires
  const bool cancelled_a = manager.CancelAndRetireActiveIfMatches(session_a);
  Expect(cancelled_a, "Matching session must return true");
  Expect(!manager.HasActiveSession(), "Active session must now be cleared");
  Expect(session_a->IsCancelled(), "session_a must be cancelled");
  Expect(manager.DeferredSessionCount() == 1, "session_a must be deferred");
  Expect(wake_count == 1, "Wake callback must be invoked");
}

void TestAsyncSessionIntegration() {
  SessionLifecycleManager manager;
  std::atomic<int> wake_count{0};
  manager.SetWakeCallback([&wake_count] { ++wake_count; });

  auto mock = std::make_unique<MockSession>();
  auto* mock_ptr = mock.get();
  auto async_session = std::make_shared<AsyncRecognitionSession>(std::move(mock));

  manager.InstallActiveSession(async_session);
  Expect(manager.HasActiveSession(), "Async session must be installed");

  // Pushing audio through AsyncRecognitionSession
  const std::vector<int16_t> pcm(160, 42);
  std::string error;
  const bool pushed = manager.PushAudio(async_session, pcm, &error);
  Expect(pushed, "Async push should succeed");

  // Polling events
  mock_ptr->QueueEvent({.kind = RecognitionEventKind::PartialText, .text = "test", .error = {}});
  // Allow async worker time to drain queue if necessary
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  manager.CancelAndRetireActive();
  Expect(manager.DeferredSessionCount() == 1, "Async session must be deferred");
  Expect(wake_count > 0, "Wake callback must have fired");

  // Reaping safely joins the async session worker thread
  const auto reaped = manager.FlushReapedSessions();
  Expect(reaped == 1, "Async session reaped and thread joined");
  Expect(manager.DeferredSessionCount() == 0, "Deferred queue cleared");
}

} // namespace

int main() {
  TestInstallAndQuery();
  TestDetachActive();
  TestAudioPushing();
  TestPollActiveEvents();
  TestFinishSessionAndCollectResult();
  TestCancelAndRetirement();
  TestCancelAndRetireActiveIfMatches();
  TestAsyncSessionIntegration();

  std::cout << "All SessionLifecycleManager tests passed!\n";
  return 0;
}
