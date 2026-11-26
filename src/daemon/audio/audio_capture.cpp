#include "daemon/audio/audio_capture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/printf.h>
#include <mutex>
#include <optional>
#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/error.h>
#include <pulse/mainloop-api.h>
#include <pulse/rtclock.h>
#include <pulse/sample.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>
#include <pulse/timeval.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/audio/pulse_device.h"
#include "common/audio/pulse_raii.h"
#include "common/i18n.h"
#include "common/utils/debug_log.h"
#include "common/utils/scope_guard.h"
#include "common/utils/time_utils.h"

namespace {

constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kChannels = 1;
// 480 mono S16LE samples = 960 bytes (~30 ms fragment latency at 16 kHz).
constexpr uint32_t kFragmentBytes = 960;
constexpr int kStartupConnectTimeoutMs = 250;
constexpr int kCaptureConnectTimeoutMs = 1500;

struct ConnectTimeoutState {
  pa_threaded_mainloop* loop{nullptr};
  bool timed_out{false};
};

void OnConnectTimeout(pa_mainloop_api* /*api*/, pa_time_event* /*event*/,
                      const struct timeval* /*tv*/, void* userdata) noexcept {
  auto* state = static_cast<ConnectTimeoutState*>(userdata);
  if (state == nullptr) {
    return;
  }
  state->timed_out = true;
  if (state->loop != nullptr) {
    pa_threaded_mainloop_signal(state->loop, 0);
  }
}

void NotifyEventFd(int fd) noexcept {
  if (fd >= 0) {
    const uint64_t val = 1;
    (void)write(fd, &val, sizeof(val));
  }
}

} // namespace

AudioCapture::~AudioCapture() {
  DestroyStream();
  DisconnectContext();
  loop_.Reset();
}

void AudioCapture::OnContextState(pa_context* ctx, void* userdata) noexcept {
  auto* self = static_cast<AudioCapture*>(userdata);
  if (self == nullptr || ctx == nullptr || !self->loop_.IsStarted()) {
    return;
  }
  const pa_context_state_t st = pa_context_get_state(ctx);
  if (st == PA_CONTEXT_READY || st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
    pa_threaded_mainloop_signal(self->loop_.Get(), 0);
  }
}

void AudioCapture::OnStreamState(pa_stream* stream, void* userdata) noexcept {
  auto* self = static_cast<AudioCapture*>(userdata);
  if (self == nullptr || stream == nullptr) {
    return;
  }
  if (pa_stream_get_state(stream) != PA_STREAM_FAILED) {
    return;
  }

  self->recording_.store(false, std::memory_order_relaxed);
  try {
    const pa_context* ctx = pa_stream_get_context(stream);
    const int err_code = ctx != nullptr ? pa_context_errno(ctx) : 0;
    const char* raw_err = err_code != 0 ? pa_strerror(err_code) : nullptr;
    const std::string err_msg = (raw_err != nullptr && raw_err[0] != '\0')
                                    ? std::string(raw_err)
                                    : std::string(_("PulseAudio stream error"));
    VLOG_ERROR("PulseAudio capture stream error: {}", err_msg);

    {
      const std::scoped_lock lock(self->callback_mutex_);
      self->pending_error_ = err_msg;
    }
    self->has_pending_error_.store(true, std::memory_order_release);
  } catch (...) {
    self->has_pending_error_.store(false, std::memory_order_relaxed);
  }
  NotifyEventFd(self->notify_fd_.load(std::memory_order_relaxed));
}

void AudioCapture::OnStreamRead(pa_stream* stream, std::size_t /*nbytes*/,
                                void* userdata) noexcept {
  auto* self = static_cast<AudioCapture*>(userdata);
  if (self == nullptr || stream == nullptr) {
    return;
  }
  self->ReadStreamFragments(stream);
}

std::size_t AudioCapture::WriteHoleSamples(std::size_t hole_samples) noexcept {
  static constexpr std::array<int16_t, 256> kZeroChunk{};
  std::size_t total_written = 0;
  std::size_t remaining = hole_samples;
  while (remaining > 0) {
    const std::size_t batch = std::min(remaining, kZeroChunk.size());
    const std::size_t written =
        ring_buffer_.Write(std::span<const int16_t>(kZeroChunk.data(), batch));
    total_written += written;
    if (written < batch) {
      dropped_samples_.fetch_add(remaining - written, std::memory_order_relaxed);
      break;
    }
    remaining -= batch;
  }
  return total_written;
}

void AudioCapture::ReadStreamFragments(pa_stream* stream) noexcept {
  std::size_t total_written = 0;

  while (pa_stream_readable_size(stream) > 0) {
    const void* data = nullptr;
    std::size_t nbytes = 0;
    if (pa_stream_peek(stream, &data, &nbytes) < 0 || nbytes == 0) {
      break;
    }
    const auto drop_packet = vinput::MakeScopeExit([stream] { (void)pa_stream_drop(stream); });

    if (!recording_.load(std::memory_order_relaxed)) {
      continue;
    }

    const std::size_t n_samples = nbytes / sizeof(int16_t);
    if (n_samples == 0) {
      continue;
    }

    if (data == nullptr) {
      total_written += WriteHoleSamples(n_samples);
      continue;
    }

    const auto* samples = static_cast<const int16_t*>(data);
    const std::size_t written = ring_buffer_.Write(std::span<const int16_t>(samples, n_samples));
    if (written < n_samples) {
      dropped_samples_.fetch_add(n_samples - written, std::memory_order_relaxed);
    }
    total_written += written;
  }

  if (total_written > 0) {
    int64_t expected_zero = 0;
    first_buffer_ns_.compare_exchange_strong(expected_zero, vinput::time::SteadyNowNs(),
                                             std::memory_order_relaxed);
    NotifyEventFd(notify_fd_.load(std::memory_order_relaxed));
  }
}

bool AudioCapture::Start(std::string* error) {
  if (loop_.IsStarted()) {
    return true;
  }

  if (!loop_.Create("vinput-capture")) {
    if (error != nullptr) {
      *error = _("failed to create PulseAudio threaded mainloop");
    }
    return false;
  }

  const int ret = loop_.Start();
  if (ret < 0) {
    if (error != nullptr) {
      *error =
          fmt::sprintf(_("failed to start PulseAudio threaded mainloop: %s"), pa_strerror(ret));
    }
    loop_.Reset();
    return false;
  }

  // Keep only the control/context connection warm at startup; never open a recording stream
  // until BeginRecording() so desktop microphone indicators stay idle.
  if (!EnsureContextConnected(nullptr, kStartupConnectTimeoutMs)) {
    VLOG_DEBUG("PulseAudio context connection deferred; will retry on capture");
  }

  return true;
}

void AudioCapture::FailContextConnectLocked(int err_code, std::string* error) {
  if (error != nullptr) {
    *error = fmt::sprintf(_("failed to connect to audio server: %s"),
                          vinput::pulse::FormatConnectionError(err_code));
  }
  context_.Reset();
}

bool AudioCapture::WaitForContextReadyLocked(pa_mainloop_api* api, int timeout_ms,
                                             std::string* error) {
  const pa_usec_t deadline_usec =
      pa_rtclock_now() + (static_cast<pa_usec_t>(std::max(timeout_ms, 50)) * PA_USEC_PER_MSEC);

  ConnectTimeoutState timeout_state{.loop = loop_.Get(), .timed_out = false};
  pa_time_event* raw_timer =
      pa_context_rttime_new(context_.Get(), deadline_usec, &OnConnectTimeout, &timeout_state);
  const vinput::pulse::ScopedTimeEvent timer_guard(api, raw_timer);

  while (!timeout_state.timed_out) {
    const pa_context_state_t st = pa_context_get_state(context_.Get());
    if (st == PA_CONTEXT_READY) {
      return true;
    }
    if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
      FailContextConnectLocked(pa_context_errno(context_.Get()), error);
      return false;
    }
    pa_threaded_mainloop_wait(loop_.Get());
  }

  FailContextConnectLocked(PA_ERR_TIMEOUT, error);
  return false;
}

bool AudioCapture::EnsureContextConnected(std::string* error, int timeout_ms) {
  if (!loop_.IsStarted()) {
    if (error != nullptr) {
      *error = _("audio capture loop is not initialized");
    }
    return false;
  }

  const vinput::pulse::ScopedThreadedMainloopLock lock(loop_.Get());
  if (context_ && pa_context_get_state(context_.Get()) == PA_CONTEXT_READY) {
    return true;
  }

  stream_.Reset();
  context_.Reset();

  pa_mainloop_api* api = pa_threaded_mainloop_get_api(loop_.Get());
  context_.Reset(pa_context_new(api, "vinput-daemon"));
  if (!context_) {
    if (error != nullptr) {
      *error = _("failed to create PulseAudio context");
    }
    return false;
  }

  pa_context_set_state_callback(context_.Get(), &OnContextState, this);
  if (pa_context_connect(context_.Get(), nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
    FailContextConnectLocked(pa_context_errno(context_.Get()), error);
    return false;
  }

  return WaitForContextReadyLocked(api, timeout_ms, error);
}

void AudioCapture::DisconnectContext() noexcept {
  if (!loop_.IsStarted()) {
    context_.Reset();
    return;
  }
  const vinput::pulse::ScopedThreadedMainloopLock lock(loop_.Get());
  context_.Reset();
}

std::string AudioCapture::CurrentTargetObject() const {
  const std::scoped_lock lock(target_mutex_);
  return target_object_;
}

bool AudioCapture::CreateAndConnectStream(
    const vinput::pulse::ResolvedCaptureTarget& resolved_target, std::string* error) {
  const vinput::pulse::ScopedThreadedMainloopLock lock(loop_.Get());
  if (!context_ || pa_context_get_state(context_.Get()) != PA_CONTEXT_READY) {
    if (error != nullptr) {
      *error = _("PulseAudio context is not ready");
    }
    return false;
  }

  const pa_sample_spec sample_spec{
      .format = PA_SAMPLE_S16LE,
      .rate = kSampleRate,
      .channels = kChannels,
  };

  vinput::pulse::ScopedStream candidate(
      pa_stream_new(context_.Get(), "vinput-capture", &sample_spec, nullptr));
  if (!candidate) {
    if (error != nullptr) {
      *error = _("failed to create PulseAudio capture stream");
    }
    return false;
  }

  pa_stream_set_state_callback(candidate.Get(), &OnStreamState, this);
  pa_stream_set_read_callback(candidate.Get(), &OnStreamRead, this);

  pa_buffer_attr buffer_attr{};
  buffer_attr.maxlength = static_cast<uint32_t>(-1);
  buffer_attr.tlength = static_cast<uint32_t>(-1);
  buffer_attr.prebuf = static_cast<uint32_t>(-1);
  buffer_attr.minreq = static_cast<uint32_t>(-1);
  buffer_attr.fragsize = kFragmentBytes;

  const char* dev =
      resolved_target.source_name.empty() ? nullptr : resolved_target.source_name.c_str();
  if (dev != nullptr) {
    VLOG_INFO("connecting PulseAudio capture stream to source={} (monitor={})", dev,
              resolved_target.is_sink_monitor);
  }

  if (pa_stream_connect_record(candidate.Get(), dev, &buffer_attr, PA_STREAM_ADJUST_LATENCY) < 0) {
    const int err_code = pa_context_errno(context_.Get());
    if (error != nullptr) {
      *error =
          fmt::sprintf(_("failed to connect PulseAudio capture stream: %s"), pa_strerror(err_code));
    }
    return false;
  }

  stream_ = std::move(candidate);
  return true;
}

void AudioCapture::DisconnectStream() noexcept {
  recording_.store(false, std::memory_order_relaxed);
  if (loop_.IsStarted()) {
    const vinput::pulse::ScopedThreadedMainloopLock lock(loop_.Get());
    const bool was_creating = stream_ && pa_stream_get_state(stream_.Get()) == PA_STREAM_CREATING;
    stream_.Reset();
    if (was_creating) {
      // pa_stream_disconnect() returns PA_ERR_BADSTATE before PA_COMMAND_REPLY arrives;
      // drop our context connection so the server tears down the pending stream cleanly.
      context_.Reset();
    }
  } else {
    stream_.Reset();
  }
}

void AudioCapture::ClearCallbackState() noexcept {
  recording_armed_ns_.store(0, std::memory_order_release);
  first_buffer_ns_.store(0, std::memory_order_release);
  first_buffer_notified_.store(false, std::memory_order_release);
  has_pending_error_.store(false, std::memory_order_release);
  const std::scoped_lock lock(callback_mutex_);
  on_first_buffer_ = nullptr;
  on_error_ = nullptr;
  pending_error_.clear();
}

void AudioCapture::DestroyStream() {
  DisconnectStream();
  ClearCallbackState();
}

void AudioCapture::DrainQueue() {
  const std::scoped_lock lock(consumer_mutex_);
  ring_buffer_.Clear();
}

void AudioCapture::ArmRecording() noexcept {
  first_buffer_ns_.store(0, std::memory_order_release);
  first_buffer_notified_.store(false, std::memory_order_release);
  recording_armed_ns_.store(vinput::time::SteadyNowNs(), std::memory_order_release);
}

bool AudioCapture::BeginRecording(std::string* error, StartTiming* timing) {
  if (!loop_.IsStarted()) {
    if (error != nullptr) {
      *error = _("audio capture loop is not initialized");
    }
    return false;
  }

  StartTiming local_timing;

  DrainQueue();
  ResetDropCounters();
  DestroyStream();

  if (!EnsureContextConnected(error, kCaptureConnectTimeoutMs)) {
    return false;
  }

  const auto resolved_target = vinput::pulse::ResolveCaptureTarget(CurrentTargetObject());

  ArmRecording();
  recording_.store(true, std::memory_order_relaxed);

  const auto create_start = std::chrono::steady_clock::now();
  if (!CreateAndConnectStream(resolved_target, error)) {
    recording_.store(false, std::memory_order_relaxed);
    recording_armed_ns_.store(0, std::memory_order_release);
    return false;
  }
  local_timing.create_stream_ms =
      vinput::time::MillisecondsBetween(create_start, std::chrono::steady_clock::now());

  VLOG_DEBUG("capture begin create_stream_ms={}", local_timing.create_stream_ms);

  if (timing != nullptr) {
    *timing = local_timing;
  }
  return true;
}

bool AudioCapture::Begin(std::string_view target, FirstBufferCallback on_first_buffer,
                         ErrorCallback on_error, std::string* error) {
  SetTargetObject(std::string(target));
  if (!BeginRecording(error, nullptr)) {
    return false;
  }
  {
    const std::scoped_lock lock(callback_mutex_);
    on_first_buffer_ = std::move(on_first_buffer);
    on_error_ = std::move(on_error);
  }
  return true;
}

void AudioCapture::DispatchPendingError() {
  if (!has_pending_error_.load(std::memory_order_acquire)) {
    return;
  }
  has_pending_error_.store(false, std::memory_order_relaxed);
  ErrorCallback err_cb;
  std::string err_msg;
  {
    const std::scoped_lock lock(callback_mutex_);
    err_cb = on_error_;
    err_msg = std::move(pending_error_);
  }
  if (err_cb) {
    err_cb(err_msg);
  }
}

std::vector<int16_t> AudioCapture::StopAndDrain() {
  DisconnectStream();
  DispatchPendingError();
  ClearCallbackState();
  const std::scoped_lock lock(consumer_mutex_);
  return ring_buffer_.DrainAll();
}

void AudioCapture::Abort() {
  EndRecording();
}

std::size_t AudioCapture::DrainAvailable(SampleConsumer consumer) {
  DispatchPendingError();

  if (!first_buffer_notified_.load(std::memory_order_relaxed)) {
    const int64_t fb_ns = first_buffer_ns_.load(std::memory_order_acquire);
    if (fb_ns > 0) {
      first_buffer_notified_.store(true, std::memory_order_relaxed);
      FirstBufferCallback cb;
      {
        const std::scoped_lock lock(callback_mutex_);
        cb = on_first_buffer_;
      }
      if (cb) {
        cb(fb_ns);
      }
    }
  }
  const std::scoped_lock lock(consumer_mutex_);
  return ring_buffer_.DrainAvailable(consumer);
}

void AudioCapture::EndRecording() {
  DestroyStream();
  DrainQueue();
}

std::vector<int16_t> AudioCapture::StopAndGetBuffer() {
  DestroyStream();

  const std::scoped_lock lock(consumer_mutex_);
  return ring_buffer_.DrainAll();
}

bool AudioCapture::IsRecording() const {
  return recording_.load(std::memory_order_relaxed);
}

void AudioCapture::SetNotifyFd(int fd) noexcept {
  notify_fd_.store(fd, std::memory_order_release);
}

std::size_t AudioCapture::DroppedSamples() const noexcept {
  return dropped_samples_.load(std::memory_order_relaxed);
}

void AudioCapture::ResetDropCounters() noexcept {
  dropped_samples_.store(0, std::memory_order_relaxed);
}

std::optional<long> AudioCapture::FirstBufferLatencyMs() const {
  return vinput::time::ElapsedMsFromNs(recording_armed_ns_.load(std::memory_order_acquire),
                                       first_buffer_ns_.load(std::memory_order_acquire));
}

void AudioCapture::SetTargetObject(std::string target_object) {
  const std::scoped_lock lock(target_mutex_);
  target_object_ = std::move(target_object);
}
