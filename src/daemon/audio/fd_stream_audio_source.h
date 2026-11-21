#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/utils/unique_fd.h"

#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/audio_ring_buffer.h"

namespace vinput::daemon::audio {

// AudioInputSource adapter reading raw S16_LE PCM (16 kHz, mono) streams from a file
// descriptor. Takes ownership of `fd` and closes it on Abort(), StopAndDrain(), or destruction.
// On a stream error the reader reports it through the error callback and stops delivering samples;
// the caller is expected to Abort() or StopAndDrain() afterwards to release the descriptor.
class FdStreamAudioSource final : public AudioInputSource {
public:
  using EofCallback = std::function<void()>;

  explicit FdStreamAudioSource(int fd, EofCallback on_eof = nullptr,
                               std::size_t capacity_seconds = 15, std::size_t sample_rate = 16000);
  ~FdStreamAudioSource() override;

  FdStreamAudioSource(const FdStreamAudioSource&) = delete;
  FdStreamAudioSource& operator=(const FdStreamAudioSource&) = delete;
  FdStreamAudioSource(FdStreamAudioSource&&) = delete;
  FdStreamAudioSource& operator=(FdStreamAudioSource&&) = delete;

  void SetEofCallback(EofCallback on_eof);

  bool Begin(std::string_view target = {}, FirstBufferCallback on_first_buffer = nullptr,
             ErrorCallback on_error = nullptr, std::string* error = nullptr) override;
  std::vector<int16_t> StopAndDrain() override;
  void Abort() override;
  std::size_t DrainAvailable(SampleConsumer consumer) override;
  void SetNotifyFd(int fd) noexcept override;
  [[nodiscard]] bool IsRecording() const override;
  [[nodiscard]] std::size_t DroppedSamples() const noexcept override;
  void ResetDropCounters() noexcept override;
  [[nodiscard]] std::optional<long> FirstBufferLatencyMs() const override;

private:
  enum class PollOutcome : std::uint8_t { Readable, Aborted, Error };
  enum class ReadStatus : std::uint8_t { Data, Eof, Aborted, Error };

  void ReaderLoop();
  PollOutcome WaitReadableOrAbort(int read_fd, int abort_fd, std::string* error_out) const;
  ReadStatus ReadChunk(int read_fd, int abort_fd, std::span<uint8_t> dst, std::size_t& bytes_read,
                       std::string* error_out) const;
  bool StreamChunks(int read_fd, int abort_fd);
  bool PushBytesAsSamples(std::span<const uint8_t> bytes, bool& has_carry, uint8_t& carry_byte,
                          std::vector<int16_t>& scratch);
  bool PushSamplesBlocking(std::span<const int16_t> samples);
  void RecordStreamError(std::string message);
  void StopReaderAndCloseFds();
  void DispatchPendingError();
  void DispatchFirstBufferIfReady();
  void DispatchEofIfReady();

  vinput::file::UniqueFd fd_;
  vinput::file::UniqueFd abort_fd_;
  AudioSampleRingBuffer ring_buffer_;

  mutable std::mutex mutex_;
  std::condition_variable space_cv_;
  std::mutex consumer_mutex_;
  std::mutex callback_mutex_;

  FirstBufferCallback on_first_buffer_;
  ErrorCallback on_error_;
  EofCallback on_eof_;
  std::string pending_error_;

  std::atomic<int> notify_fd_{-1};
  std::atomic<bool> recording_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> eof_{false};
  std::atomic<bool> eof_notified_{false};
  std::atomic<bool> first_buffer_notified_{false};
  std::atomic<bool> has_pending_error_{false};
  std::atomic<int64_t> recording_armed_ns_{0};
  std::atomic<int64_t> first_buffer_ns_{0};
  std::atomic<std::size_t> dropped_samples_{0};

  std::thread reader_thread_;
};

} // namespace vinput::daemon::audio
