#include "daemon/asr/runtime/async_recognition_session.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

AsyncRecognitionSession::AsyncRecognitionSession(std::unique_ptr<RecognitionSession> backend,
                                                 std::size_t max_queued_samples)
    : backend_(std::move(backend)), max_queued_samples_(max_queued_samples) {
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
}

bool AsyncRecognitionSession::PushAudio(std::span<const int16_t> pcm, std::string* error) {
  // On overflow the queued chunks are moved here and destroyed after the
  // lock is released below, so freeing them does not extend the time the
  // capture callback holds mutex_ (the queue is bounded, see header).
  std::deque<std::vector<int16_t>> discarded;
  {
    const std::scoped_lock lock(mutex_);
    if (cancel_requested_ || finish_requested_ || complete_) {
      if (error != nullptr) {
        *error = error_.empty() ? "Streaming session is no longer recording." : error_;
      }
      return false;
    }
    if (!pcm.empty() && (queued_samples_ >= max_queued_samples_ ||
                         pcm.size() > max_queued_samples_ - queued_samples_)) {
      // Bounded queue is full: fail the session explicitly instead of
      // silently dropping audio and later reporting an incomplete recording
      // as successful. Blocking here would stall the capture callback, which
      // must stay realtime-safe, so wake the worker to run the existing
      // cancel/failure path instead of waiting for queue space.
      if (dropped_chunks_ < std::numeric_limits<std::size_t>::max()) {
        ++dropped_chunks_;
      }
      const std::size_t max_samples = std::numeric_limits<std::size_t>::max();
      dropped_samples_ =
          pcm.size() > max_samples - dropped_samples_ ? max_samples : dropped_samples_ + pcm.size();
      error_ = "Streaming recognition queue overflowed; audio chunk was dropped.";
      cancel_requested_ = true;
      discarded = std::move(queue_);
      queue_.clear();
      queued_samples_ = 0;
      wake_.notify_one();
      if (error != nullptr) {
        *error = error_;
      }
      return false;
    }
    if (!pcm.empty()) {
      queue_.emplace_back(pcm.begin(), pcm.end());
      queued_samples_ += pcm.size();
      wake_.notify_one();
    }
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool AsyncRecognitionSession::Finish(std::string* error) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!complete_ && !cancel_requested_) {
    finish_requested_ = true;
    wake_.notify_one();
  }
  complete_cv_.wait(lock, [this] { return complete_; });
  if (error != nullptr) {
    *error = error_;
  }
  return success_;
}

void AsyncRecognitionSession::Cancel() {
  {
    const std::scoped_lock lock(mutex_);
    if (complete_ || cancel_requested_) {
      return;
    }
    cancel_requested_ = true;
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

std::optional<std::string> AsyncRecognitionSession::FailureError() const {
  const std::scoped_lock lock(mutex_);
  if (!backend_failed_) {
    return std::nullopt;
  }
  return error_;
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
    complete_ = true;
    success_ = success;
    error_ = std::move(error);
  }
  complete_cv_.notify_all();
}

void AsyncRecognitionSession::Run() {
  while (true) {
    std::vector<int16_t> chunk;
    bool finish = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock,
                 [this] { return cancel_requested_ || finish_requested_ || !queue_.empty(); });
      if (cancel_requested_) {
        break;
      }
      if (!queue_.empty()) {
        chunk = std::move(queue_.front());
        queue_.pop_front();
        queued_samples_ -= chunk.size();
      } else if (finish_requested_) {
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
      auto backend_events = backend_->PollEvents();
      {
        const std::scoped_lock lock(mutex_);
        error_ = error.empty() ? "Streaming backend rejected audio." : error;
        backend_failed_ = true;
        cancel_requested_ = true;
        queue_.clear();
        queued_samples_ = 0;
      }
      // Publish the failure before appending events: AppendEvents wakes the
      // controller, which must already see FailureError() so it filters the
      // Error event below and notifies only once via FailActiveSession.
      AppendEvents(std::move(backend_events));
      // Wake again for the failure itself: the woken thread re-reads
      // FailureError(), so this still surfaces when the backend emitted no
      // Error event (and merges harmlessly if it did).
      InvokeWakeCallback();
      break;
    }
    AppendEvents(backend_->PollEvents());
  }

  // Cancel (or backend failure): cancel the backend exactly once on this
  // worker thread, so backend calls are always serialized on one thread.
  backend_->Cancel();
  AppendEvents(backend_->PollEvents());
  std::string error;
  {
    const std::scoped_lock lock(mutex_);
    error = error_;
  }
  Complete(false, std::move(error));
}

} // namespace vinput::daemon::asr
