#include "daemon/audio/fd_stream_audio_source.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/audio/wav_probe.h"
#include "common/utils/debug_log.h"
#include "common/utils/time_utils.h"
#include "common/utils/unique_fd.h"

#include "daemon/audio/audio_input_source.h"

namespace vinput::daemon::audio {

namespace {

constexpr std::size_t kInitialProbeBytes = 1024;
constexpr std::size_t kReadChunkBytes = 1600; // 50ms of 16kHz 16-bit mono PCM

bool StartsWithRiffPrefix(std::span<const uint8_t> buf) noexcept {
  if (buf.size() < vinput::audio::kFourCcSize) {
    return false;
  }
  return std::memcmp(buf.data(), "RIFF", vinput::audio::kFourCcSize) == 0;
}

} // namespace

FdStreamAudioSource::FdStreamAudioSource(int fd, EofCallback on_eof, std::size_t capacity_seconds,
                                         std::size_t sample_rate)
    : fd_(fd), ring_buffer_(capacity_seconds, sample_rate), on_eof_(std::move(on_eof)) {}

FdStreamAudioSource::~FdStreamAudioSource() {
  Abort();
}

void FdStreamAudioSource::SetEofCallback(EofCallback on_eof) {
  const std::scoped_lock lock(callback_mutex_);
  on_eof_ = std::move(on_eof);
}

bool FdStreamAudioSource::Begin(std::string_view /*target*/, FirstBufferCallback on_first_buffer,
                                ErrorCallback on_error, std::string* error) {
  if (!fd_.IsValid()) {
    if (error != nullptr) {
      *error = "invalid stream file descriptor";
    }
    return false;
  }
  if (recording_.load(std::memory_order_acquire)) {
    if (error != nullptr) {
      *error = "stream audio source is already active";
    }
    return false;
  }

  abort_fd_.Reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!abort_fd_.IsValid()) {
    if (error != nullptr) {
      *error = std::string("failed to create stream abort eventfd: ") + std::strerror(errno);
    }
    return false;
  }

  {
    const std::scoped_lock lock(callback_mutex_);
    on_first_buffer_ = std::move(on_first_buffer);
    on_error_ = std::move(on_error);
    pending_error_.clear();
  }

  ring_buffer_.Clear();
  stop_requested_.store(false, std::memory_order_release);
  eof_.store(false, std::memory_order_release);
  eof_notified_.store(false, std::memory_order_release);
  first_buffer_notified_.store(false, std::memory_order_release);
  has_pending_error_.store(false, std::memory_order_release);
  first_buffer_ns_.store(0, std::memory_order_release);
  dropped_samples_.store(0, std::memory_order_relaxed);
  recording_armed_ns_.store(vinput::time::SteadyNowNs(), std::memory_order_release);
  recording_.store(true, std::memory_order_release);

  reader_thread_ = std::thread([this] { ReaderLoop(); });
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void FdStreamAudioSource::RecordStreamError(std::string message) {
  VLOG_WARN("fd stream audio source error: {}", message);
  {
    const std::scoped_lock lock(callback_mutex_);
    pending_error_ = std::move(message);
  }
  has_pending_error_.store(true, std::memory_order_release);
  vinput::file::SignalEventFd(notify_fd_.load(std::memory_order_acquire));
}

FdStreamAudioSource::PollOutcome
FdStreamAudioSource::WaitReadableOrAbort(int read_fd, int abort_fd, std::string* error_out) const {
  while (!stop_requested_.load(std::memory_order_acquire)) {
    std::array<pollfd, 2> fds = {{
        {.fd = read_fd, .events = POLLIN, .revents = 0},
        {.fd = abort_fd, .events = POLLIN, .revents = 0},
    }};
    const int rc = poll(fds.data(), fds.size(), -1);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_out != nullptr) {
        *error_out = std::string("stream poll failed: ") + std::strerror(errno);
      }
      return PollOutcome::Error;
    }
    if ((fds[1].revents & POLLIN) != 0 || stop_requested_.load(std::memory_order_acquire)) {
      return PollOutcome::Aborted;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
      return PollOutcome::Readable;
    }
  }
  return PollOutcome::Aborted;
}

FdStreamAudioSource::ReadStatus FdStreamAudioSource::ReadChunk(int read_fd, int abort_fd,
                                                               std::span<uint8_t> dst,
                                                               std::size_t& bytes_read,
                                                               std::string* error_out) const {
  bytes_read = 0;
  while (!stop_requested_.load(std::memory_order_acquire)) {
    const auto poll_res = WaitReadableOrAbort(read_fd, abort_fd, error_out);
    if (poll_res == PollOutcome::Aborted) {
      return ReadStatus::Aborted;
    }
    if (poll_res == PollOutcome::Error) {
      return ReadStatus::Error;
    }

    const ssize_t n = read(read_fd, dst.data(), dst.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_out != nullptr) {
        *error_out = std::string("stream read failed: ") + std::strerror(errno);
      }
      return ReadStatus::Error;
    }
    if (n == 0) {
      return ReadStatus::Eof;
    }
    bytes_read = static_cast<std::size_t>(n);
    return ReadStatus::Data;
  }
  return ReadStatus::Aborted;
}

bool FdStreamAudioSource::ReadInitialProbe(int read_fd, int abort_fd,
                                           std::vector<uint8_t>& probe_buf, bool& reached_eof,
                                           std::string* error_out) {
  probe_buf.clear();
  probe_buf.reserve(kInitialProbeBytes);
  reached_eof = false;

  std::array<uint8_t, kInitialProbeBytes> temp{};
  while (probe_buf.size() < kInitialProbeBytes) {
    std::size_t bytes_read = 0;
    const auto status = ReadChunk(
        read_fd, abort_fd, std::span<uint8_t>(temp.data(), kInitialProbeBytes - probe_buf.size()),
        bytes_read, error_out);
    if (status == ReadStatus::Aborted || status == ReadStatus::Error) {
      return false;
    }
    if (status == ReadStatus::Eof) {
      reached_eof = true;
      return true;
    }

    probe_buf.insert(probe_buf.end(), temp.data(), temp.data() + bytes_read);
    if (probe_buf.size() >= vinput::audio::kFourCcSize && !StartsWithRiffPrefix(probe_buf)) {
      return true;
    }
  }
  return true;
}

bool FdStreamAudioSource::PushSamplesBlocking(std::span<const int16_t> samples) {
  while (!samples.empty()) {
    std::size_t written = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      space_cv_.wait(lock, [this] {
        return stop_requested_.load(std::memory_order_acquire) ||
               ring_buffer_.Available() < ring_buffer_.Capacity();
      });
      if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
      }
      written = ring_buffer_.Write(samples);
    }

    if (written > 0) {
      int64_t expected_zero = 0;
      first_buffer_ns_.compare_exchange_strong(expected_zero, vinput::time::SteadyNowNs(),
                                               std::memory_order_release);
      vinput::file::SignalEventFd(notify_fd_.load(std::memory_order_acquire));
      samples = samples.subspan(written);
    }
  }
  return true;
}

bool FdStreamAudioSource::PushBytesAsSamples(std::span<const uint8_t> bytes, bool& has_carry,
                                             uint8_t& carry_byte, std::vector<int16_t>& scratch) {
  if (bytes.empty()) {
    return true;
  }

  scratch.clear();
  std::size_t idx = 0;
  if (has_carry) {
    const std::array<uint8_t, 2> pair{carry_byte, bytes[0]};
    int16_t sample = 0;
    std::memcpy(&sample, pair.data(), sizeof(sample));
    scratch.push_back(sample);
    has_carry = false;
    idx = 1;
  }

  const std::size_t remaining = bytes.size() - idx;
  const std::size_t pair_count = remaining / sizeof(int16_t);
  if (pair_count > 0) {
    const std::size_t old_size = scratch.size();
    scratch.resize(old_size + pair_count);
    std::memcpy(scratch.data() + old_size, bytes.data() + idx, pair_count * sizeof(int16_t));
    idx += pair_count * sizeof(int16_t);
  }

  if (idx < bytes.size()) {
    carry_byte = bytes[idx];
    has_carry = true;
  }

  return PushSamplesBlocking(scratch);
}

bool FdStreamAudioSource::StreamRemainingChunks(int read_fd, int abort_fd, bool& has_carry,
                                                uint8_t& carry_byte,
                                                std::vector<int16_t>& scratch) {
  std::array<uint8_t, kReadChunkBytes> chunk{};
  std::string read_error;
  while (!stop_requested_.load(std::memory_order_acquire)) {
    std::size_t bytes_read = 0;
    const auto status = ReadChunk(read_fd, abort_fd, chunk, bytes_read, &read_error);
    if (status == ReadStatus::Aborted) {
      return false;
    }
    if (status == ReadStatus::Error) {
      RecordStreamError(std::move(read_error));
      return false;
    }
    if (status == ReadStatus::Eof) {
      return true;
    }

    const auto bytes = std::span<const uint8_t>(chunk.data(), bytes_read);
    if (!PushBytesAsSamples(bytes, has_carry, carry_byte, scratch)) {
      return false;
    }
  }
  return false;
}

void FdStreamAudioSource::ReaderLoop() {
  const int read_fd = fd_.Get();
  const int abort_fd = abort_fd_.Get();

  std::vector<uint8_t> probe_buf;
  bool reached_eof = false;
  std::string read_error;
  if (!ReadInitialProbe(read_fd, abort_fd, probe_buf, reached_eof, &read_error)) {
    if (!read_error.empty() && !stop_requested_.load(std::memory_order_acquire)) {
      RecordStreamError(std::move(read_error));
    }
    return;
  }

  bool has_carry = false;
  uint8_t carry_byte = 0;
  std::vector<int16_t> sample_scratch;
  sample_scratch.reserve(kReadChunkBytes / sizeof(int16_t) + 1);

  if (!probe_buf.empty()) {
    const std::size_t offset = vinput::audio::DetectWavDataOffset(probe_buf);
    if (offset < probe_buf.size()) {
      const auto initial_pcm = std::span<const uint8_t>(probe_buf).subspan(offset);
      if (!PushBytesAsSamples(initial_pcm, has_carry, carry_byte, sample_scratch)) {
        return;
      }
    }
  }

  if (reached_eof ||
      StreamRemainingChunks(read_fd, abort_fd, has_carry, carry_byte, sample_scratch)) {
    eof_.store(true, std::memory_order_release);
    vinput::file::SignalEventFd(notify_fd_.load(std::memory_order_acquire));
  }
}

void FdStreamAudioSource::StopReaderAndCloseFds() {
  recording_.store(false, std::memory_order_release);
  {
    const std::scoped_lock lock(mutex_);
    stop_requested_.store(true, std::memory_order_release);
  }
  vinput::file::SignalEventFd(abort_fd_.Get());
  space_cv_.notify_all();
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }
  fd_.Reset();
  abort_fd_.Reset();
}

void FdStreamAudioSource::DispatchPendingError() {
  if (!has_pending_error_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
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

void FdStreamAudioSource::DispatchFirstBufferIfReady() {
  if (first_buffer_notified_.load(std::memory_order_acquire)) {
    return;
  }
  const int64_t fb_ns = first_buffer_ns_.load(std::memory_order_acquire);
  if (fb_ns <= 0) {
    return;
  }
  first_buffer_notified_.store(true, std::memory_order_release);
  FirstBufferCallback cb;
  {
    const std::scoped_lock lock(callback_mutex_);
    cb = on_first_buffer_;
  }
  if (cb) {
    cb(fb_ns);
  }
}

void FdStreamAudioSource::DispatchEofIfReady() {
  if (!eof_.load(std::memory_order_acquire) || ring_buffer_.Available() > 0) {
    return;
  }
  if (eof_notified_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  EofCallback cb;
  {
    const std::scoped_lock lock(callback_mutex_);
    cb = on_eof_;
  }
  if (cb) {
    cb();
  }
}

std::vector<int16_t> FdStreamAudioSource::StopAndDrain() {
  StopReaderAndCloseFds();
  DispatchPendingError();
  const std::scoped_lock lock(consumer_mutex_);
  return ring_buffer_.DrainAll();
}

void FdStreamAudioSource::Abort() {
  StopReaderAndCloseFds();
  const std::scoped_lock lock(consumer_mutex_);
  ring_buffer_.Clear();
  const std::scoped_lock cb_lock(callback_mutex_);
  on_first_buffer_ = nullptr;
  on_error_ = nullptr;
  on_eof_ = nullptr;
  pending_error_.clear();
}

std::size_t FdStreamAudioSource::DrainAvailable(SampleConsumer consumer) {
  DispatchPendingError();
  DispatchFirstBufferIfReady();

  std::size_t drained = 0;
  {
    const std::scoped_lock lock(consumer_mutex_);
    drained = ring_buffer_.DrainAvailable(std::move(consumer));
  }
  if (drained > 0) {
    {
      const std::scoped_lock lock(mutex_);
    }
    space_cv_.notify_all();
  }

  DispatchEofIfReady();
  return drained;
}

void FdStreamAudioSource::SetNotifyFd(int fd) noexcept {
  notify_fd_.store(fd, std::memory_order_release);
}

bool FdStreamAudioSource::IsRecording() const {
  return recording_.load(std::memory_order_acquire);
}

std::size_t FdStreamAudioSource::DroppedSamples() const noexcept {
  return dropped_samples_.load(std::memory_order_relaxed);
}

void FdStreamAudioSource::ResetDropCounters() noexcept {
  dropped_samples_.store(0, std::memory_order_relaxed);
}

std::optional<long> FdStreamAudioSource::FirstBufferLatencyMs() const {
  return vinput::time::ElapsedMsFromNs(recording_armed_ns_.load(std::memory_order_acquire),
                                       first_buffer_ns_.load(std::memory_order_acquire));
}

} // namespace vinput::daemon::audio
