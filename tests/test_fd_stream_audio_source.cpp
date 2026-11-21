#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "daemon/audio/fd_stream_audio_source.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

using vinput::daemon::audio::FdStreamAudioSource;

bool WaitForEventFd(int efd, int timeout_ms = 2000) {
  pollfd pfd{.fd = efd, .events = POLLIN, .revents = 0};
  const int rc = poll(&pfd, 1, timeout_ms);
  if (rc <= 0 || (pfd.revents & POLLIN) == 0) {
    return false;
  }
  uint64_t val = 0;
  while (read(efd, &val, sizeof(val)) == sizeof(val)) {
  }
  return true;
}

std::vector<uint8_t> BuildWavHeader(uint32_t data_bytes) {
  std::vector<uint8_t> hdr(44, 0);
  std::memcpy(hdr.data(), "RIFF", 4);
  const uint32_t riff_size = 36 + data_bytes;
  std::memcpy(hdr.data() + 4, &riff_size, 4);
  std::memcpy(hdr.data() + 8, "WAVEfmt ", 8);
  const uint32_t fmt_size = 16;
  std::memcpy(hdr.data() + 16, &fmt_size, 4);
  const uint16_t audio_fmt = 1;
  const uint16_t channels = 1;
  const uint32_t sample_rate = 16000;
  const uint32_t byte_rate = 32000;
  const uint16_t block_align = 2;
  const uint16_t bits_per_sample = 16;
  std::memcpy(hdr.data() + 20, &audio_fmt, 2);
  std::memcpy(hdr.data() + 22, &channels, 2);
  std::memcpy(hdr.data() + 24, &sample_rate, 4);
  std::memcpy(hdr.data() + 28, &byte_rate, 4);
  std::memcpy(hdr.data() + 32, &block_align, 2);
  std::memcpy(hdr.data() + 34, &bits_per_sample, 2);
  std::memcpy(hdr.data() + 36, "data", 4);
  std::memcpy(hdr.data() + 40, &data_bytes, 4);
  return hdr;
}

void TestInvalidFdRejected() {
  FdStreamAudioSource source(-1);
  std::string error;
  TEST_ASSERT(!source.Begin({}, nullptr, nullptr, &error));
  TEST_ASSERT(!error.empty());
  TEST_ASSERT(!source.IsRecording());
}

void TestWavHeaderAndOddByteBoundary() {
  std::array<int, 2> pipe_fds{-1, -1};
  TEST_ASSERT(pipe(pipe_fds.data()) == 0);
  const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  TEST_ASSERT(efd >= 0);

  std::atomic<int> first_buf_count{0};
  std::atomic<bool> eof_called{false};
  std::atomic<bool> error_called{false};

  FdStreamAudioSource source(pipe_fds[0], [&eof_called] { eof_called.store(true); });
  source.SetNotifyFd(efd);

  std::string error;
  TEST_ASSERT(source.Begin(
      {}, [&first_buf_count](int64_t) { first_buf_count.fetch_add(1); },
      [&error_called](std::string_view) { error_called.store(true); }, &error));

  const std::array<int16_t, 4> expected{111, -222, 333, -444};
  auto wav = BuildWavHeader(static_cast<uint32_t>(expected.size() * sizeof(int16_t)));
  const auto pcm_bytes = std::as_bytes(std::span(expected));
  wav.reserve(wav.size() + pcm_bytes.size());
  for (const std::byte b : pcm_bytes) {
    wav.push_back(std::to_integer<uint8_t>(b));
  }

  // Write split across an odd byte boundary (47 bytes, then remainder)
  TEST_ASSERT(write(pipe_fds[1], wav.data(), 47) == 47);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  TEST_ASSERT(write(pipe_fds[1], wav.data() + 47, wav.size() - 47) ==
              static_cast<ssize_t>(wav.size() - 47));
  close(pipe_fds[1]);

  std::vector<int16_t> collected;
  for (int i = 0; i < 20 && !eof_called.load(); ++i) {
    (void)WaitForEventFd(efd, 100);
    source.DrainAvailable([&collected](std::span<const int16_t> chunk) {
      collected.insert(collected.end(), chunk.begin(), chunk.end());
    });
  }

  TEST_ASSERT(eof_called.load());
  TEST_ASSERT(!error_called.load());
  TEST_ASSERT(first_buf_count.load() == 1);
  TEST_ASSERT(collected.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    TEST_ASSERT(collected[i] == expected.at(i));
  }
  TEST_ASSERT(source.FirstBufferLatencyMs().has_value());

  const auto tail = source.StopAndDrain();
  TEST_ASSERT(tail.empty());
  TEST_ASSERT(!source.IsRecording());
  close(efd);
}

void TestRawPcmCarryAcrossReads() {
  std::array<int, 2> pipe_fds{-1, -1};
  TEST_ASSERT(pipe(pipe_fds.data()) == 0);
  const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  TEST_ASSERT(efd >= 0);

  FdStreamAudioSource source(pipe_fds[0]);
  source.SetNotifyFd(efd);
  TEST_ASSERT(source.Begin());

  std::vector<int16_t> collected;
  const auto collect = [&source, &collected] {
    source.DrainAvailable([&collected](std::span<const int16_t> chunk) {
      collected.insert(collected.end(), chunk.begin(), chunk.end());
    });
  };

  // 5 bytes = two full samples + one dangling byte; the dangling byte must survive
  // until the next read completes the third sample.
  const std::array<int16_t, 3> expected{100, -200, 300};
  const auto bytes = std::as_bytes(std::span(expected));
  TEST_ASSERT(write(pipe_fds[1], bytes.data(), 5) == 5);
  for (int i = 0; i < 50 && collected.size() < 2; ++i) {
    (void)WaitForEventFd(efd, 100);
    collect();
  }
  TEST_ASSERT(collected.size() == 2);

  TEST_ASSERT(write(pipe_fds[1], bytes.data() + 5, 1) == 1);
  close(pipe_fds[1]);
  for (int i = 0; i < 50 && collected.size() < expected.size(); ++i) {
    (void)WaitForEventFd(efd, 100);
    collect();
  }
  TEST_ASSERT(collected.size() == expected.size());
  TEST_ASSERT(collected[0] == 100 && collected[1] == -200 && collected[2] == 300);

  source.StopAndDrain();
  close(efd);
}

void TestEmptyStreamEof() {
  std::array<int, 2> pipe_fds{-1, -1};
  TEST_ASSERT(pipe(pipe_fds.data()) == 0);
  const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  TEST_ASSERT(efd >= 0);

  std::atomic<int> first_buf_count{0};
  std::atomic<bool> eof_called{false};

  FdStreamAudioSource source(pipe_fds[0], [&eof_called] { eof_called.store(true); });
  source.SetNotifyFd(efd);
  TEST_ASSERT(source.Begin({}, [&first_buf_count](int64_t) { first_buf_count.fetch_add(1); }));

  close(pipe_fds[1]);
  for (int i = 0; i < 20 && !eof_called.load(); ++i) {
    (void)WaitForEventFd(efd, 100);
    source.DrainAvailable([](std::span<const int16_t>) {});
  }

  TEST_ASSERT(eof_called.load());
  TEST_ASSERT(first_buf_count.load() == 0);
  TEST_ASSERT(source.StopAndDrain().empty());
  close(efd);
}

void TestAbortInterruptsBlockedPipe() {
  std::array<int, 2> pipe_fds{-1, -1};
  TEST_ASSERT(pipe(pipe_fds.data()) == 0);

  FdStreamAudioSource source(pipe_fds[0]);
  TEST_ASSERT(source.Begin());
  TEST_ASSERT(source.IsRecording());

  // Writer stays open and silent; Abort() must return promptly without hanging.
  const auto start = std::chrono::steady_clock::now();
  source.Abort();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  TEST_ASSERT(elapsed < std::chrono::milliseconds(500));
  TEST_ASSERT(!source.IsRecording());

  close(pipe_fds[1]);
}

void TestBackpressurePreservesAllSamples() {
  std::array<int, 2> pipe_fds{-1, -1};
  TEST_ASSERT(pipe(pipe_fds.data()) == 0);
  const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  TEST_ASSERT(efd >= 0);

  // Tiny 1-second capacity at 100 Hz = 100 samples; write 500 samples.
  std::atomic<bool> eof_called{false};
  FdStreamAudioSource source(pipe_fds[0], [&eof_called] { eof_called.store(true); }, 1, 100);
  source.SetNotifyFd(efd);
  TEST_ASSERT(source.Begin());

  std::vector<int16_t> input(500);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<int16_t>(i + 1);
  }

  std::thread writer([&] {
    const auto raw = std::as_bytes(std::span(input));
    const std::size_t total_bytes = raw.size();
    std::size_t sent = 0;
    while (sent < total_bytes) {
      const ssize_t n = write(pipe_fds[1], raw.data() + sent, total_bytes - sent);
      if (n <= 0) {
        break;
      }
      sent += static_cast<std::size_t>(n);
    }
    close(pipe_fds[1]);
  });

  std::vector<int16_t> collected;
  for (int i = 0; i < 50 && !eof_called.load(); ++i) {
    (void)WaitForEventFd(efd, 100);
    source.DrainAvailable([&collected](std::span<const int16_t> chunk) {
      collected.insert(collected.end(), chunk.begin(), chunk.end());
    });
  }

  writer.join();
  TEST_ASSERT(eof_called.load());
  TEST_ASSERT(source.DroppedSamples() == 0);
  TEST_ASSERT(collected == input);
  source.StopAndDrain();
  close(efd);
}

void TestStreamErrorStopsImmediately() {
  std::array<int, 2> pipe_fds{-1, -1};
  TEST_ASSERT(pipe(pipe_fds.data()) == 0);
  const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  TEST_ASSERT(efd >= 0);

  // Pass the write end as read_fd and close the read end: poll() reports POLLERR and
  // the subsequent read() on the write-only fd fails with EBADF.
  std::atomic<bool> error_called{false};
  std::string error_msg;
  FdStreamAudioSource source(pipe_fds[1]);
  close(pipe_fds[0]);
  source.SetNotifyFd(efd);
  TEST_ASSERT(source.Begin({}, nullptr, [&](std::string_view msg) {
    error_called.store(true);
    error_msg = std::string(msg);
  }));

  for (int i = 0; i < 20 && !error_called.load(); ++i) {
    (void)WaitForEventFd(efd, 100);
    source.DrainAvailable([](std::span<const int16_t>) {});
  }

  TEST_ASSERT(error_called.load());
  TEST_ASSERT(!error_msg.empty());
  source.StopAndDrain();
  close(efd);
}

} // namespace

int main() {
  std::printf("Running test_fd_stream_audio_source...\n");
  TestInvalidFdRejected();
  TestWavHeaderAndOddByteBoundary();
  TestRawPcmCarryAcrossReads();
  TestEmptyStreamEof();
  TestAbortInterruptsBlockedPipe();
  TestBackpressurePreservesAllSamples();
  TestStreamErrorStopsImmediately();
  std::printf("All test_fd_stream_audio_source passed!\n");
  return 0;
}
