#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/mainloop-api.h>
#include <pulse/mainloop.h>
#include <pulse/operation.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>

namespace vinput::pulse {

// RAII wrapper for single-threaded pa_mainloop handles used in bounded control queries.
class ScopedMainloop {
public:
  ScopedMainloop() : loop_(pa_mainloop_new()) {}
  ~ScopedMainloop() { Reset(); }

  ScopedMainloop(const ScopedMainloop&) = delete;
  ScopedMainloop& operator=(const ScopedMainloop&) = delete;

  ScopedMainloop(ScopedMainloop&& other) noexcept : loop_(other.loop_) { other.loop_ = nullptr; }
  ScopedMainloop& operator=(ScopedMainloop&& other) noexcept {
    if (this != &other) {
      Reset();
      loop_ = other.loop_;
      other.loop_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] pa_mainloop* Get() const noexcept { return loop_; }
  [[nodiscard]] explicit operator bool() const noexcept { return loop_ != nullptr; }

  void Reset() noexcept {
    if (loop_ != nullptr) {
      pa_mainloop_free(loop_);
      loop_ = nullptr;
    }
  }

private:
  pa_mainloop* loop_{nullptr};
};

// RAII wrapper for pa_threaded_mainloop handles; stops the loop thread (unlocked) before freeing.
class ScopedThreadedMainloop {
public:
  ScopedThreadedMainloop() = default;
  ~ScopedThreadedMainloop() { Reset(); }

  ScopedThreadedMainloop(const ScopedThreadedMainloop&) = delete;
  ScopedThreadedMainloop& operator=(const ScopedThreadedMainloop&) = delete;
  ScopedThreadedMainloop(ScopedThreadedMainloop&&) = delete;
  ScopedThreadedMainloop& operator=(ScopedThreadedMainloop&&) = delete;

  bool Create(const char* thread_name = nullptr) noexcept {
    Reset();
    loop_ = pa_threaded_mainloop_new();
    if (loop_ != nullptr && thread_name != nullptr) {
      pa_threaded_mainloop_set_name(loop_, thread_name);
    }
    return loop_ != nullptr;
  }

  int Start() noexcept {
    if (loop_ == nullptr) {
      return -1;
    }
    if (started_) {
      return 0;
    }
    const int ret = pa_threaded_mainloop_start(loop_);
    if (ret >= 0) {
      started_ = true;
    }
    return ret;
  }

  // Must be called WITHOUT the threaded mainloop lock held.
  void Stop() noexcept {
    if (loop_ != nullptr && started_) {
      pa_threaded_mainloop_stop(loop_);
      started_ = false;
    }
  }

  // Must be called WITHOUT the threaded mainloop lock held.
  void Reset() noexcept {
    Stop();
    if (loop_ != nullptr) {
      pa_threaded_mainloop_free(loop_);
      loop_ = nullptr;
    }
  }

  [[nodiscard]] pa_threaded_mainloop* Get() const noexcept { return loop_; }
  [[nodiscard]] bool IsStarted() const noexcept { return started_; }
  [[nodiscard]] explicit operator bool() const noexcept { return loop_ != nullptr; }

private:
  pa_threaded_mainloop* loop_{nullptr};
  bool started_{false};
};

// RAII wrapper for pa_operation handles; cancels running operations before unreferencing.
class ScopedOperation {
public:
  ScopedOperation() = default;
  explicit ScopedOperation(pa_operation* op) noexcept : op_(op) {}
  ~ScopedOperation() { Reset(); }

  ScopedOperation(const ScopedOperation&) = delete;
  ScopedOperation& operator=(const ScopedOperation&) = delete;

  ScopedOperation(ScopedOperation&& other) noexcept : op_(other.op_) { other.op_ = nullptr; }
  ScopedOperation& operator=(ScopedOperation&& other) noexcept {
    if (this != &other) {
      Reset();
      op_ = other.op_;
      other.op_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] pa_operation* Get() const noexcept { return op_; }
  [[nodiscard]] explicit operator bool() const noexcept { return op_ != nullptr; }

  void Reset(pa_operation* next = nullptr) noexcept {
    if (op_ != nullptr) {
      if (pa_operation_get_state(op_) == PA_OPERATION_RUNNING) {
        pa_operation_cancel(op_);
      }
      pa_operation_unref(op_);
    }
    op_ = next;
  }

private:
  pa_operation* op_{nullptr};
};

// RAII wrapper for pa_context handles; clears callbacks and disconnects cleanly before unref.
class ScopedContext {
public:
  ScopedContext() = default;
  explicit ScopedContext(pa_context* ctx) noexcept : ctx_(ctx) {}
  ~ScopedContext() { Reset(); }

  ScopedContext(const ScopedContext&) = delete;
  ScopedContext& operator=(const ScopedContext&) = delete;

  ScopedContext(ScopedContext&& other) noexcept : ctx_(other.ctx_) { ctx_ = nullptr; }
  ScopedContext& operator=(ScopedContext&& other) noexcept {
    if (this != &other) {
      Reset();
      ctx_ = other.ctx_;
      other.ctx_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] pa_context* Get() const noexcept { return ctx_; }
  [[nodiscard]] explicit operator bool() const noexcept { return ctx_ != nullptr; }

  void Reset(pa_context* next = nullptr) noexcept {
    if (ctx_ != nullptr) {
      pa_context_set_state_callback(ctx_, nullptr, nullptr);
      const pa_context_state_t st = pa_context_get_state(ctx_);
      if (st != PA_CONTEXT_UNCONNECTED && st != PA_CONTEXT_FAILED && st != PA_CONTEXT_TERMINATED) {
        pa_context_disconnect(ctx_);
      }
      pa_context_unref(ctx_);
    }
    ctx_ = next;
  }

private:
  pa_context* ctx_{nullptr};
};

// RAII wrapper for pa_stream handles; clears callbacks and disconnects cleanly before unref.
class ScopedStream {
public:
  ScopedStream() = default;
  explicit ScopedStream(pa_stream* stream) noexcept : stream_(stream) {}
  ~ScopedStream() { Reset(); }

  ScopedStream(const ScopedStream&) = delete;
  ScopedStream& operator=(const ScopedStream&) = delete;

  ScopedStream(ScopedStream&& other) noexcept : stream_(other.stream_) { other.stream_ = nullptr; }
  ScopedStream& operator=(ScopedStream&& other) noexcept {
    if (this != &other) {
      Reset();
      stream_ = other.stream_;
      other.stream_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] pa_stream* Get() const noexcept { return stream_; }
  [[nodiscard]] explicit operator bool() const noexcept { return stream_ != nullptr; }

  void Reset(pa_stream* next = nullptr) noexcept {
    if (stream_ != nullptr) {
      pa_stream_set_read_callback(stream_, nullptr, nullptr);
      pa_stream_set_state_callback(stream_, nullptr, nullptr);
      const pa_stream_state_t st = pa_stream_get_state(stream_);
      if (st != PA_STREAM_UNCONNECTED && st != PA_STREAM_FAILED && st != PA_STREAM_TERMINATED) {
        (void)pa_stream_disconnect(stream_);
      }
      pa_stream_unref(stream_);
    }
    stream_ = next;
  }

private:
  pa_stream* stream_{nullptr};
};

// RAII wrapper for pa_time_event created on a pa_mainloop_api.
class ScopedTimeEvent {
public:
  ScopedTimeEvent(pa_mainloop_api* api, pa_time_event* event) noexcept : api_(api), event_(event) {}
  ~ScopedTimeEvent() {
    if (api_ != nullptr && event_ != nullptr && api_->time_free != nullptr) {
      api_->time_free(event_);
    }
  }

  ScopedTimeEvent(const ScopedTimeEvent&) = delete;
  ScopedTimeEvent& operator=(const ScopedTimeEvent&) = delete;
  ScopedTimeEvent(ScopedTimeEvent&&) = delete;
  ScopedTimeEvent& operator=(ScopedTimeEvent&&) = delete;

private:
  pa_mainloop_api* api_{nullptr};
  pa_time_event* event_{nullptr};
};

// Scoped lock guard for pa_threaded_mainloop; never invoke inside the RT callback itself.
class ScopedThreadedMainloopLock {
public:
  explicit ScopedThreadedMainloopLock(pa_threaded_mainloop* loop) noexcept : loop_(loop) {
    if (loop_ != nullptr) {
      pa_threaded_mainloop_lock(loop_);
    }
  }
  ~ScopedThreadedMainloopLock() {
    if (loop_ != nullptr) {
      pa_threaded_mainloop_unlock(loop_);
    }
  }

  ScopedThreadedMainloopLock(const ScopedThreadedMainloopLock&) = delete;
  ScopedThreadedMainloopLock& operator=(const ScopedThreadedMainloopLock&) = delete;
  ScopedThreadedMainloopLock(ScopedThreadedMainloopLock&&) = delete;
  ScopedThreadedMainloopLock& operator=(ScopedThreadedMainloopLock&&) = delete;

private:
  pa_threaded_mainloop* loop_{nullptr};
};

// Bounded-timeout synchronous PulseAudio control session for introspection and volume queries.
class PulseSyncSession {
public:
  PulseSyncSession() = default;
  ~PulseSyncSession() {
    ctx_.Reset();
    loop_.Reset();
  }

  PulseSyncSession(const PulseSyncSession&) = delete;
  PulseSyncSession& operator=(const PulseSyncSession&) = delete;
  PulseSyncSession(PulseSyncSession&&) = delete;
  PulseSyncSession& operator=(PulseSyncSession&&) = delete;

  bool Connect(const char* app_name, std::chrono::steady_clock::time_point deadline);
  bool RunOperation(pa_operation* raw_op, std::chrono::steady_clock::time_point deadline);

  [[nodiscard]] pa_context* Context() const noexcept { return ctx_.Get(); }
  [[nodiscard]] int LastError() const noexcept { return last_error_; }

private:
  template <typename Predicate>
  bool StepUntil(Predicate&& done, std::chrono::steady_clock::time_point deadline) {
    while (!done()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return false;
      }
      const auto remaining_us =
          std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
      const int timeout_us = static_cast<int>(std::clamp<int64_t>(remaining_us, 1000, 50'000));
      if (pa_mainloop_prepare(loop_.Get(), timeout_us) < 0) {
        return done();
      }
      if (pa_mainloop_poll(loop_.Get()) < 0) {
        return done();
      }
      if (pa_mainloop_dispatch(loop_.Get()) < 0) {
        return done();
      }
    }
    return true;
  }

  ScopedMainloop loop_;
  ScopedContext ctx_;
  int last_error_{0};
};

} // namespace vinput::pulse
