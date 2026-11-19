#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "daemon/asr/runtime/async_recognition_session.h"
#include "daemon/asr/runtime/recognition_contract.h"

namespace {

using vinput::daemon::asr::AsyncRecognitionSession;
using vinput::daemon::asr::RecognitionEvent;
using vinput::daemon::asr::RecognitionEventKind;
using vinput::daemon::asr::RecognitionSession;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Async session regression: " << message << '\n';
    std::abort();
  }
}

// Observable fake backend state, kept alive by the test after the session
// (and its owned backend) are destroyed.
struct FakeState {
  std::mutex mutex;
  std::condition_variable gate_cv;
  bool gate_open = true;
  bool entered_push = false;
  std::vector<int16_t> received;
  int push_calls = 0;
  int finish_calls = 0;
  int cancel_calls = 0;
  bool fail_push = false;
};

class FakeSession final : public RecognitionSession {
public:
  explicit FakeSession(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    ++state_->push_calls;
    state_->entered_push = true;
    state_->gate_cv.notify_all();
    state_->gate_cv.wait(lock, [this] { return state_->gate_open; });
    if (state_->fail_push) {
      // No Error event on purpose: the session must surface this via its
      // failure state, not via an event.
      if (error != nullptr) {
        *error = "fake backend rejected audio.";
      }
      return false;
    }
    state_->received.insert(state_->received.end(), pcm.begin(), pcm.end());
    pending_.push_back({.kind = RecognitionEventKind::PartialText, .text = "p", .error = {}});
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool Finish(std::string* error) override {
    const std::scoped_lock lock(state_->mutex);
    ++state_->finish_calls;
    pending_.push_back({.kind = RecognitionEventKind::Completed, .text = {}, .error = {}});
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void Cancel() override {
    const std::scoped_lock lock(state_->mutex);
    ++state_->cancel_calls;
    pending_.push_back({.kind = RecognitionEventKind::Completed, .text = {}, .error = {}});
  }

  std::vector<RecognitionEvent> PollEvents() override {
    const std::scoped_lock lock(state_->mutex);
    auto events = std::move(pending_);
    pending_.clear();
    return events;
  }

private:
  std::shared_ptr<FakeState> state_;
  std::vector<RecognitionEvent> pending_;
};

std::shared_ptr<FakeState> MakeState(bool gate_open) {
  auto state = std::make_shared<FakeState>();
  state->gate_open = gate_open;
  return state;
}

void WaitEntered(const std::shared_ptr<FakeState>& state) {
  std::unique_lock<std::mutex> lock(state->mutex);
  const bool ok = state->gate_cv.wait_for(lock, std::chrono::seconds(5),
                                          [&state] { return state->entered_push; });
  Expect(ok, "worker entered backend PushAudio");
}

void OpenGate(const std::shared_ptr<FakeState>& state) {
  {
    const std::scoped_lock lock(state->mutex);
    state->gate_open = true;
  }
  state->gate_cv.notify_all();
}

int CountCompleted(const std::vector<RecognitionEvent>& events) {
  int count = 0;
  for (const auto& event : events) {
    if (event.kind == RecognitionEventKind::Completed) {
      ++count;
    }
  }
  return count;
}

struct WakeRecorder {
  std::mutex mutex;
  std::condition_variable cv;
  int wakes = 0;

  void Record() {
    {
      const std::scoped_lock lock(mutex);
      ++wakes;
    }
    cv.notify_all();
  }

  void WaitAtLeast(int count) {
    std::unique_lock<std::mutex> lock(mutex);
    const bool ok =
        cv.wait_for(lock, std::chrono::seconds(5), [this, count] { return wakes >= count; });
    Expect(ok, "wake callback fired");
  }
};

void TestWakeOnPartialWithoutMoreAudio() {
  auto state = MakeState(true);
  auto recorder = std::make_shared<WakeRecorder>();
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
  // Querying the session from inside the callback proves the callback is
  // not invoked while the session mutex is held (that would deadlock here).
  session.SetWakeCallback([&session, recorder] {
    (void)session.DroppedChunks();
    recorder->Record();
  });
  std::string error;
  Expect(session.PushAudio(std::vector<int16_t>{1, 2}, &error), "push single chunk");
  // No further audio is fed: the worker's Partial must arrive via the wake.
  recorder->WaitAtLeast(1);
  const auto events = session.PollEvents();
  Expect(events.size() == 1 && events.front().kind == RecognitionEventKind::PartialText &&
             events.front().text == "p",
         "partial produced without new audio is pollable after wake");
  Expect(session.Finish(&error), "finish after wake test");
}

void TestWakeOnBackendFailureWithoutErrorEvent() {
  auto state = MakeState(true);
  state->fail_push = true;
  auto recorder = std::make_shared<WakeRecorder>();
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
  session.SetWakeCallback([recorder] { recorder->Record(); });
  std::string error;
  Expect(session.PushAudio(std::vector<int16_t>{1}, &error), "enqueue before worker failure");
  recorder->WaitAtLeast(1);
  // Backend push failures are synthesized into an Error event (Issue #709):
  // the consumer reads the event stream, no side channel.
  const auto events = session.PollEvents();
  const bool saw_error = std::ranges::any_of(
      events, [](const RecognitionEvent& ev) { return ev.kind == RecognitionEventKind::Error; });
  Expect(saw_error, "backend failure surfaces as an Error event via PollEvents");
  Expect(!session.Finish(&error), "finish reports worker failure");
  Expect(error == "fake backend rejected audio.", "failure error preserved for Finish waiter");
  const auto final_events = session.PollEvents();
  const auto total_completed = CountCompleted(events) + CountCompleted(final_events);
  Expect(total_completed == 1, "Completed exactly once after failure");
  // Terminal events drain once; nothing is replayed.
  Expect(session.PollEvents().empty(), "failure events are not replayed");
}

void TestFinishFlushesInOrderExactlyOnce() {
  auto state = MakeState(true);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
  std::string error;
  Expect(session.PushAudio(std::vector<int16_t>{1, 2}, &error), "push 1");
  Expect(session.PushAudio(std::vector<int16_t>{3}, &error), "push 2");
  Expect(session.Finish(&error), "finish succeeds after flushing queue");
  {
    const std::scoped_lock lock(state->mutex);
    Expect(state->received == std::vector<int16_t>({1, 2, 3}), "chunks delivered in order");
    Expect(state->finish_calls == 1, "backend Finish called once");
  }
  Expect(CountCompleted(session.PollEvents()) == 1, "Completed emitted exactly once");
  Expect(session.PollEvents().empty(), "events are not replayed");
  Expect(session.Finish(&error), "second Finish returns cached result");
  const std::scoped_lock lock(state->mutex);
  Expect(state->finish_calls == 1, "second Finish must not re-finish backend");
}

void TestQueueOverflowFailsSession() {
  auto state = MakeState(false);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 4);
  std::string error;
  Expect(session.PushAudio(std::vector<int16_t>{1, 2}, &error), "push in-flight chunk");
  WaitEntered(state); // Worker is inside the backend with chunk 1.
  Expect(session.PushAudio(std::vector<int16_t>{3, 4}, &error), "push queues chunk");
  Expect(session.PushAudio(std::vector<int16_t>{5, 6}, &error), "push fills queue to bound");
  error.clear();
  Expect(!session.PushAudio(std::vector<int16_t>{7, 8}, &error), "overflow push fails");
  Expect(!error.empty(), "overflow push reports a non-empty error");
  Expect(session.DroppedChunks() == 1, "overflowing chunk is counted, not blocked");
  Expect(session.DroppedSamples() == 2, "dropped samples counted");
  OpenGate(state);
  error.clear();
  Expect(!session.Finish(&error), "Finish after overflow reports failure");
  Expect(!error.empty(), "Finish after overflow reports a non-empty error");
  const auto events = session.PollEvents();
  const bool saw_error = std::ranges::any_of(
      events, [](const RecognitionEvent& ev) { return ev.kind == RecognitionEventKind::Error; });
  Expect(saw_error, "overflow surfaces as an Error event via PollEvents");
  const std::scoped_lock lock(state->mutex);
  Expect(state->received == std::vector<int16_t>({1, 2}),
         "queued and dropped chunks are never delivered after overflow");
  Expect(state->finish_calls == 0, "backend Finish not called after overflow");
  Expect(state->cancel_calls == 1, "backend Cancel called exactly once after overflow");
}

void TestQueueOverflowOversizedChunk() {
  auto state = MakeState(true);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 4);
  std::string error;
  Expect(!session.PushAudio(std::vector<int16_t>{1, 2, 3, 4, 5}, &error),
         "chunk larger than total capacity fails");
  Expect(!error.empty(), "oversized chunk reports a non-empty error");
  Expect(session.DroppedChunks() == 1, "oversized chunk is counted as dropped");
  Expect(session.DroppedSamples() == 5, "oversized chunk samples counted");
  Expect(!session.Finish(&error), "Finish after oversized chunk reports failure");
  Expect(!error.empty(), "Finish after oversized chunk reports a non-empty error");
  const auto events = session.PollEvents();
  const bool saw_error = std::ranges::any_of(
      events, [](const RecognitionEvent& ev) { return ev.kind == RecognitionEventKind::Error; });
  Expect(saw_error, "oversized chunk surfaces as an Error event via PollEvents");
}

void TestQueueZeroCapacityFails() {
  auto state = MakeState(true);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 0);
  std::string error;
  Expect(!session.PushAudio(std::vector<int16_t>{1}, &error), "zero-capacity push fails");
  Expect(!error.empty(), "zero-capacity push reports a non-empty error");
  Expect(session.DroppedChunks() == 1, "zero-capacity chunk is counted as dropped");
  Expect(session.DroppedSamples() == 1, "zero-capacity samples counted");
  Expect(!session.Finish(&error), "Finish with zero capacity reports failure");
  Expect(!error.empty(), "zero-capacity Finish reports a non-empty error");
  const auto events = session.PollEvents();
  const bool saw_error = std::ranges::any_of(
      events, [](const RecognitionEvent& ev) { return ev.kind == RecognitionEventKind::Error; });
  Expect(saw_error, "zero capacity push surfaces as an Error event via PollEvents");
}

void TestCancelClearsQueueAndCompletesOnce() {
  auto state = MakeState(false);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
  std::string error;
  Expect(session.PushAudio(std::vector<int16_t>{1}, &error), "push in-flight");
  WaitEntered(state);
  Expect(session.PushAudio(std::vector<int16_t>{2}, &error), "push queued");
  session.Cancel();
  OpenGate(state);
  Expect(!session.Finish(&error), "Finish after Cancel reports failure");
  Expect(error == "Recognition session was cancelled.",
         "Finish after Cancel reports cancellation error");
  {
    const std::scoped_lock lock(state->mutex);
    Expect(state->received == std::vector<int16_t>({1}), "queued chunk is discarded by Cancel");
    Expect(state->cancel_calls == 1, "backend Cancel called exactly once");
    Expect(state->finish_calls == 0, "backend Finish not called after Cancel");
  }
  Expect(CountCompleted(session.PollEvents()) == 1, "Completed exactly once after Cancel");
}

void TestDestructorCancelsAndJoinsPromptly() {
  auto state = MakeState(true);
  const auto started = std::chrono::steady_clock::now();
  {
    AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
    std::string error;
    Expect(session.PushAudio(std::vector<int16_t>{7, 8}, &error), "push before destruction");
  } // Destructor: Cancel + join; backend must not outlive the worker call.
  const auto elapsed = std::chrono::steady_clock::now() - started;
  Expect(elapsed < std::chrono::seconds(5), "destructor joins promptly");
  const std::scoped_lock lock(state->mutex);
  Expect(state->cancel_calls == 1, "destructor cancels backend exactly once");
}

// Equivalent of the controller's deferred retirement at session level:
// after Cancel, a local reference released on another thread must not join
// the worker while the deferred holder still owns the session. The gate
// keeps the backend inside PushAudio; the releaser must report completion
// (signalled via condition variable, not a fixed-ms performance gate)
// while the gate is still closed. Only clearing the deferred holder on
// this thread may join, and repeated Cancel must stay exactly-once.
void TestDeferredReleaseDoesNotJoinOnReleasingThread() {
  auto state = MakeState(false);
  auto session =
      std::make_shared<AsyncRecognitionSession>(std::make_unique<FakeSession>(state), 1024);
  std::string error;
  Expect(session->PushAudio(std::vector<int16_t>{1}, &error), "push in-flight");
  WaitEntered(state); // Worker is blocked inside backend PushAudio.
  Expect(session->PushAudio(std::vector<int16_t>{2}, &error), "push queued");
  session->Cancel();
  session->Cancel(); // Idempotent: must not cause a second backend Cancel.

  std::vector<std::shared_ptr<AsyncRecognitionSession>> deferred;
  deferred.push_back(session);

  std::mutex release_mutex;
  std::condition_variable release_cv;
  bool released = false;
  std::thread releaser([&] {
    session.reset();
    {
      const std::scoped_lock lock(release_mutex);
      released = true;
    }
    release_cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(release_mutex);
    const bool ok = release_cv.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
    Expect(ok, "releasing a non-last reference returns while backend is gated");
  }
  releaser.join();
  {
    const std::scoped_lock lock(state->mutex);
    Expect(state->cancel_calls == 0, "backend Cancel not yet run while PushAudio is gated");
  }

  OpenGate(state);
  deferred.clear(); // Main-thread retirement: destructor joins worker here.
  const std::scoped_lock lock(state->mutex);
  Expect(state->received == std::vector<int16_t>({1}), "queued chunk discarded by Cancel");
  Expect(state->cancel_calls == 1, "backend Cancel exactly once after repeated Cancel");
  Expect(state->finish_calls == 0, "backend Finish not called after Cancel");
}

void TestPushAudioFloatFallback() {
  auto state = MakeState(true);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
  std::string error;
  const std::vector<float> float_samples = {0.0F, 1.0F, -1.0F, 0.5F,
                                            std::numeric_limits<float>::quiet_NaN()};
  Expect(session.PushAudio(float_samples, &error), "push float audio succeeded");
  Expect(error.empty(), "push float audio has no error");
  Expect(session.Finish(&error), "session finish succeeded");
  {
    const std::scoped_lock lock(state->mutex);
    const std::vector<int16_t> expected = {0, 32767, -32768, 16384, 0};
    Expect(state->received == expected,
           "float samples accurately converted to int16 by default fallback with NaN mapped to 0");
  }

  const std::vector<float> empty_samples;
  Expect(!session.PushAudio(empty_samples, &error),
         "push empty float audio after finish must fail");
}

void TestPushAudioFloatEmpty() {
  auto state = MakeState(true);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
  std::string error;
  const std::vector<float> empty_samples;
  Expect(session.PushAudio(empty_samples, &error), "push empty float audio succeeded");
  Expect(error.empty(), "push empty float audio has no error");
}

void TestDestructionOrderDestroysSessionBeforeOwner() {
  struct LifetimeTracker {
    bool session_destroyed = false;
    bool owner_destroyed = false;
  };
  auto tracker = std::make_shared<LifetimeTracker>();

  class TrackingSession : public RecognitionSession {
  public:
    explicit TrackingSession(std::shared_ptr<LifetimeTracker> tracker)
        : tracker_(std::move(tracker)) {}
    ~TrackingSession() override {
      tracker_->session_destroyed = true;
      Expect(!tracker_->owner_destroyed,
             "backend owner must still be alive when session is destroyed");
    }
    bool PushAudio(std::span<const int16_t>, std::string*) override { return true; }
    bool Finish(std::string*) override { return true; }
    void Cancel() override {}
    std::vector<RecognitionEvent> PollEvents() override { return {}; }

  private:
    std::shared_ptr<LifetimeTracker> tracker_;
  };

  class TrackingBackend : public AsrBackend {
  public:
    explicit TrackingBackend(std::shared_ptr<LifetimeTracker> tracker)
        : tracker_(std::move(tracker)) {}
    ~TrackingBackend() override {
      tracker_->owner_destroyed = true;
      Expect(tracker_->session_destroyed,
             "session must be destroyed before backend owner is destroyed");
    }
    [[nodiscard]] BackendDescriptor Describe() const override { return {}; }
    std::unique_ptr<RecognitionSession> CreateSession(std::string*) override { return nullptr; }
    bool Warmup(std::string*) override { return true; }

  private:
    std::shared_ptr<LifetimeTracker> tracker_;
  };

  {
    auto backend = std::make_shared<TrackingBackend>(tracker);
    auto session = std::make_unique<TrackingSession>(tracker);
    AsyncRecognitionSession async_session(std::move(session), 1024, std::move(backend));
  }
  Expect(tracker->session_destroyed, "session was destroyed");
  Expect(tracker->owner_destroyed, "owner was destroyed");
}

} // namespace

int main() {
  TestWakeOnPartialWithoutMoreAudio();
  TestWakeOnBackendFailureWithoutErrorEvent();
  TestFinishFlushesInOrderExactlyOnce();
  TestQueueOverflowFailsSession();
  TestQueueOverflowOversizedChunk();
  TestQueueZeroCapacityFails();
  TestCancelClearsQueueAndCompletesOnce();
  TestDestructorCancelsAndJoinsPromptly();
  TestDeferredReleaseDoesNotJoinOnReleasingThread();
  TestPushAudioFloatFallback();
  TestPushAudioFloatEmpty();
  TestDestructionOrderDestroysSessionBeforeOwner();
  std::cout << "Async recognition session tests passed\n";
}
