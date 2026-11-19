#include "daemon/asr/runtime/async_recognition_session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/utils/debug_log.h"

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

AsyncRecognitionSession::AsyncRecognitionSession(std::unique_ptr<RecognitionSession> backend,
                                                 std::size_t max_queued_samples,
                                                 std::shared_ptr<AsrBackend> backend_owner)
    : backend_owner_(std::move(backend_owner)), backend_(std::move(backend)),
      max_queued_samples_(max_queued_samples) {
  worker_ = std::thread([this] { Run(); });
}

AsyncRecognitionSession::~AsyncRecognitionSession() {
  // Ask the worker to stop, then join it here so the backend session is never
  // destroyed while the worker is calling into it. The queue is cleared by
  // Cancel(), so the join waits only for the at-most-one in-flight chunk.
  Cancel();
  if (worker_.joinable()) {
    worker_.join();
  }
  backend_.reset();
}

bool AsyncRecognitionSession::IsAcceptingAudioLocked(std::string* error) const {
  if (phase_ != Phase::Accepting) {
    if (error != nullptr) {
      *error = error_.empty() ? "Streaming session is no longer recording." : error_;
    }
    return false;
  }
  return true;
}

bool AsyncRecognitionSession::HasCapacityLocked(std::size_t incoming_samples) const {
  if (queued_samples_ >= max_queued_samples_) {
    return false;
  }
  return incoming_samples <= max_queued_samples_ - queued_samples_;
}

void AsyncRecognitionSession::RecordDroppedSamplesLocked(std::size_t incoming_samples) {
  if (dropped_chunks_ < std::numeric_limits<std::size_t>::max()) {
    ++dropped_chunks_;
  }
  const std::size_t max_samples = std::numeric_limits<std::size_t>::max();
  if (incoming_samples > max_samples - dropped_samples_) {
    dropped_samples_ = max_samples;
  } else {
    dropped_samples_ += incoming_samples;
  }
}

void AsyncRecognitionSession::HandleQueueOverflowLocked(std::size_t incoming_samples,
                                                        std::deque<std::vector<int16_t>>& discarded,
                                                        std::string* error) {
  // Bounded queue is full: fail the session explicitly instead of
  // silently dropping audio and later reporting an incomplete recording
  // as successful. Blocking here would stall the capture callback, which
  // must stay realtime-safe, so wake the worker to run the existing
  // cancel/failure path instead of waiting for queue space.
  RecordDroppedSamplesLocked(incoming_samples);
  VLOG_WARN("recognition queue overflow: dropped {} chunks ({} samples)", dropped_chunks_,
            dropped_samples_);
  error_ = "Streaming recognition queue overflowed; audio chunk was dropped.";
  phase_ = Phase::Cancelling;
  discarded = std::move(queue_);
  queue_.clear();
  queued_samples_ = 0;
  wake_.notify_one();
  if (error != nullptr) {
    *error = error_;
  }
}

bool AsyncRecognitionSession::PushAudio(std::span<const int16_t> pcm, std::string* error) {
  // Every success path returns a clean error sink and every failure path below
  // overwrites it, so clear once on entry instead of at each exit.
  if (error != nullptr) {
    error->clear();
  }
  // On overflow the queued chunks are moved here and destroyed after the
  // lock is released below, so freeing them does not extend the time the
  // capture callback holds mutex_ (the queue is bounded, see header).
  std::deque<std::vector<int16_t>> discarded;
  {
    const std::scoped_lock lock(mutex_);
    if (!IsAcceptingAudioLocked(error)) {
      return false;
    }
    if (pcm.empty()) {
      return true;
    }
    if (!HasCapacityLocked(pcm.size())) {
      HandleQueueOverflowLocked(pcm.size(), discarded, error);
      return false;
    }
    queue_.emplace_back(pcm.begin(), pcm.end());
    queued_samples_ += pcm.size();
    wake_.notify_one();
  }
  return true;
}

bool AsyncRecognitionSession::Finish(std::string* error) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (phase_ != Phase::Complete && phase_ != Phase::Cancelling) {
    phase_ = Phase::Finishing;
    wake_.notify_one();
  }
  complete_cv_.wait(lock, [this] { return phase_ == Phase::Complete; });
  if (error != nullptr) {
    *error = error_;
  }
  return success_;
}

void AsyncRecognitionSession::Cancel() {
  {
    const std::scoped_lock lock(mutex_);
    if (phase_ == Phase::Complete || phase_ == Phase::Cancelling) {
      return;
    }
    phase_ = Phase::Cancelling;
    queue_.clear();
    queued_samples_ = 0;
  }
  wake_.notify_one();
  if (backend_) {
    backend_->Interrupt();
  }
}

void AsyncRecognitionSession::Interrupt() noexcept {
  wake_.notify_one();
  if (backend_) {
    backend_->Interrupt();
  }
}

std::vector<RecognitionEvent> AsyncRecognitionSession::PollEvents() {
  const std::scoped_lock lock(mutex_);
  auto events = std::move(events_);
  events_.clear();
  return events;
}

std::size_t AsyncRecognitionSession::DroppedChunks() const {
  const std::scoped_lock lock(mutex_);
  return dropped_chunks_;
}

std::size_t AsyncRecognitionSession::DroppedSamples() const {
  const std::scoped_lock lock(mutex_);
  return dropped_samples_;
}

void AsyncRecognitionSession::SetWakeCallback(WakeCallback callback) {
  const std::scoped_lock lock(mutex_);
  wake_callback_ = std::move(callback);
}

void AsyncRecognitionSession::EnsureErrorEvent(std::vector<RecognitionEvent>& events) {
  const bool has_error = std::ranges::any_of(
      events, [](const RecognitionEvent& ev) { return ev.kind == RecognitionEventKind::Error; });
  const std::scoped_lock lock(mutex_);
  if (has_error) {
    error_emitted_ = true;
    return;
  }
  if (!error_emitted_) {
    const bool in_queue = std::ranges::any_of(
        events_, [](const RecognitionEvent& ev) { return ev.kind == RecognitionEventKind::Error; });
    if (!in_queue) {
      events.push_back({RecognitionEventKind::Error, {}, error_});
    }
    error_emitted_ = true;
  }
}

void AsyncRecognitionSession::InvokeWakeCallback() {
  WakeCallback callback;
  {
    const std::scoped_lock lock(mutex_);
    callback = wake_callback_;
  }
  // Invoked without mutex_ held: the callback may query this session, and no
  // lock ordering is imposed on the woken thread's own wake primitive.
  if (callback) {
    callback();
  }
}

void AsyncRecognitionSession::AppendEvents(std::vector<RecognitionEvent> events) {
  if (events.empty()) {
    return;
  }
  {
    const std::scoped_lock lock(mutex_);
    for (auto& event : events) {
      events_.push_back(std::move(event));
    }
  }
  InvokeWakeCallback();
}

void AsyncRecognitionSession::Complete(bool success, std::string error) {
  {
    const std::scoped_lock lock(mutex_);
    phase_ = Phase::Complete;
    success_ = success;
    error_ = std::move(error);
  }
  complete_cv_.notify_all();
}

void AsyncRecognitionSession::HandleBackendFailure(const std::string& push_error) {
  auto backend_events = backend_->PollEvents();
  {
    const std::scoped_lock lock(mutex_);
    error_ = push_error.empty() ? "Streaming backend rejected audio." : push_error;
    phase_ = Phase::Cancelling;
    queue_.clear();
    queued_samples_ = 0;
  }
  // Backend push failures surface as a single Error event through
  // PollEvents() (Issue #709): if the backend did not already queue one,
  // synthesize it so the consumer sees the failure via the event stream
  // instead of a parallel failure channel.
  EnsureErrorEvent(backend_events);
  AppendEvents(std::move(backend_events));
}

void AsyncRecognitionSession::FinalizeCancellation() {
  // Cancel (or backend failure / queue overflow): cancel the backend exactly once
  // on this worker thread, so backend calls are always serialized on one thread.
  backend_->Cancel();
  auto cancel_events = backend_->PollEvents();
  std::string error;
  bool is_failure = false;
  {
    const std::scoped_lock lock(mutex_);
    if (!error_.empty()) {
      is_failure = true;
    } else {
      error_ = "Recognition session was cancelled.";
    }
    error = error_;
  }
  if (is_failure) {
    EnsureErrorEvent(cancel_events);
  }
  AppendEvents(std::move(cancel_events));
  Complete(false, std::move(error));
}

void AsyncRecognitionSession::Run() {
  while (true) {
    std::vector<int16_t> chunk;
    bool finish = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return phase_ != Phase::Accepting || !queue_.empty(); });
      if (phase_ == Phase::Cancelling) {
        break;
      }
      if (!queue_.empty()) {
        chunk = std::move(queue_.front());
        queue_.pop_front();
        queued_samples_ -= chunk.size();
      } else if (phase_ == Phase::Finishing) {
        finish = true;
      }
    }

    std::string error;
    if (finish) {
      // Queue is drained: finish the backend exactly once and forward its
      // final events (including its single Completed) untouched.
      const bool ok = backend_->Finish(&error);
      AppendEvents(backend_->PollEvents());
      Complete(ok, std::move(error));
      return;
    }

    if (!backend_->PushAudio(chunk, &error)) {
      HandleBackendFailure(error);
      break;
    }
    AppendEvents(backend_->PollEvents());
  }

  FinalizeCancellation();
}

} // namespace vinput::daemon::asr
