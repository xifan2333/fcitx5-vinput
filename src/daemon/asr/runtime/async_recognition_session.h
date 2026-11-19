#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

// Wraps a streaming session so PushAudio/PollEvents run on one worker thread
// instead of the audio capture callback. The callback only enqueues an owned
// copy. The queue is bounded (default 10 s at 16 kHz): when full, the incoming
// chunk is counted as dropped and the session fails explicitly rather than
// blocking the realtime callback or silently losing audio.
class AsyncRecognitionSession final : public RecognitionSession {
public:
  // Worker/consumer lifecycle. Backend failure is expressed as an Error event
  // through PollEvents() rather than a distinct state (Issue #709), so the
  // consumer only ever reads events and Finish()'s return value.
  enum class Phase : std::uint8_t {
    Accepting,  // Worker waits for queued audio; PushAudio is accepted.
    Finishing,  // Finish() requested; only the final drain runs.
    Cancelling, // Cancel() requested or backend push failed; teardown runs.
    Complete,   // Worker finished; final state is frozen.
  };

  // Non-blocking wake-up invoked on the worker thread after new events are
  // appended or the backend fails. The callback must only signal another
  // thread (for example write an eventfd); it must never call UI/D-Bus code.
  // It is copied out from under mutex_ and invoked after that lock is
  // released, so it may safely query this session.
  using WakeCallback = std::function<void()>;

  explicit AsyncRecognitionSession(std::unique_ptr<RecognitionSession> backend,
                                   std::size_t max_queued_samples = std::size_t{16000} * 10,
                                   std::shared_ptr<AsrBackend> backend_owner = nullptr);
  ~AsyncRecognitionSession() override;

  AsyncRecognitionSession(const AsyncRecognitionSession&) = delete;
  AsyncRecognitionSession& operator=(const AsyncRecognitionSession&) = delete;
  AsyncRecognitionSession(AsyncRecognitionSession&&) = delete;
  AsyncRecognitionSession& operator=(AsyncRecognitionSession&&) = delete;

  using RecognitionSession::PushAudio;

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override;
  bool Finish(std::string* error) override;
  void Cancel() override;
  void Interrupt() noexcept override;
  std::vector<RecognitionEvent> PollEvents() override;

  void SetWakeCallback(WakeCallback callback);

  [[nodiscard]] std::size_t DroppedChunks() const;
  [[nodiscard]] std::size_t DroppedSamples() const;

private:
  void Run();
  void FinalizeCancellation();
  void HandleBackendFailure(const std::string& push_error);
  void AppendEvents(std::vector<RecognitionEvent> events);
  void Complete(bool success, std::string error);
  void InvokeWakeCallback();
  // Ensures the event batch carries an Error event; synthesizes one from
  // error_ when the backend emitted none (Issue #709).
  void EnsureErrorEvent(std::vector<RecognitionEvent>& events);
  [[nodiscard]] bool IsAcceptingAudioLocked(std::string* error) const;
  [[nodiscard]] bool HasCapacityLocked(std::size_t incoming_samples) const;
  void RecordDroppedSamplesLocked(std::size_t incoming_samples);
  void HandleQueueOverflowLocked(std::size_t incoming_samples,
                                 std::deque<std::vector<int16_t>>& discarded, std::string* error);

  // Keeps the owning AsrBackend alive for the worker thread's lifetime (Issue
  // #709) so a backend reload that destroys the previous backend cannot leave
  // the worker calling into a dangling recognizer. Declared before backend_ so
  // backend_ is destroyed first.
  std::shared_ptr<AsrBackend> backend_owner_;
  std::unique_ptr<RecognitionSession> backend_;
  const std::size_t max_queued_samples_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable complete_cv_;
  std::deque<std::vector<int16_t>> queue_;
  std::vector<RecognitionEvent> events_;
  std::size_t queued_samples_ = 0;
  std::size_t dropped_chunks_ = 0;
  std::size_t dropped_samples_ = 0;
  Phase phase_ = Phase::Accepting;
  bool success_ = false;
  std::string error_;
  WakeCallback wake_callback_;
  bool error_emitted_ = false;

  std::thread worker_; // Joined in the destructor before members are torn down.
};

} // namespace vinput::daemon::asr
