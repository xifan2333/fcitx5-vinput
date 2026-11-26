#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

#include "daemon/audio/audio_capture.h"
#include "daemon/audio/mock_audio_input_source.h"

namespace {

class ScopedEnvVar {
public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    const char* prev = std::getenv(name);
    if (prev != nullptr) {
      old_value_ = std::string(prev);
    }
    if (value != nullptr) {
      (void)::setenv(name, value, 1);
    } else {
      (void)::unsetenv(name);
    }
  }

  ~ScopedEnvVar() {
    if (old_value_.has_value()) {
      (void)::setenv(name_.c_str(), old_value_->c_str(), 1);
    } else {
      (void)::unsetenv(name_.c_str());
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
  ScopedEnvVar(ScopedEnvVar&&) = delete;
  ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

using vinput::daemon::audio::MockAudioInputSource;

void TestMockLifecycle() {
  MockAudioInputSource source(5, 16000);
  TEST_ASSERT(!source.IsRecording());
  TEST_ASSERT(source.DroppedSamples() == 0);

  std::string error;
  TEST_ASSERT(source.Begin("mock-device", nullptr, nullptr, &error));
  TEST_ASSERT(source.IsRecording());
  TEST_ASSERT(source.LastTarget() == "mock-device");
  TEST_ASSERT(error.empty());

  const std::array<int16_t, 320> samples{};
  source.PushSamples(samples);

  std::vector<int16_t> drained;
  const std::size_t count = source.DrainAvailable([&drained](std::span<const int16_t> chunk) {
    drained.insert(drained.end(), chunk.begin(), chunk.end());
  });
  TEST_ASSERT(count == 320);
  TEST_ASSERT(drained.size() == 320);

  const auto leftover = source.StopAndDrain();
  TEST_ASSERT(leftover.empty());
  TEST_ASSERT(!source.IsRecording());
}

void TestMockFirstBufferCallback() {
  MockAudioInputSource source(5, 16000);
  int first_buffer_calls = 0;
  int64_t recorded_ns = 0;

  auto on_first = [&first_buffer_calls, &recorded_ns](int64_t ns) {
    ++first_buffer_calls;
    recorded_ns = ns;
  };

  TEST_ASSERT(source.Begin("default", on_first, nullptr, nullptr));
  TEST_ASSERT(first_buffer_calls == 0);
  TEST_ASSERT(!source.FirstBufferLatencyMs().has_value());

  const std::array<int16_t, 160> chunk1{};
  source.PushSamples(chunk1);
  TEST_ASSERT(first_buffer_calls == 1);
  TEST_ASSERT(recorded_ns > 0);
  const auto latency = source.FirstBufferLatencyMs();
  TEST_ASSERT(latency.has_value());
  TEST_ASSERT(latency.value_or(-1) >= 0);

  const std::array<int16_t, 160> chunk2{};
  source.PushSamples(chunk2);
  TEST_ASSERT(first_buffer_calls == 1);

  source.StopAndDrain();
}

void TestMockSimulatedLatency() {
  MockAudioInputSource source(5, 16000);
  source.SetSimulatedLatencyMs(42);
  const auto sim_latency = source.FirstBufferLatencyMs();
  TEST_ASSERT(sim_latency.has_value());
  TEST_ASSERT(sim_latency.value_or(0) == 42);

  source.SetSimulatedLatencyMs(std::nullopt);
  TEST_ASSERT(!source.FirstBufferLatencyMs().has_value());
}

void TestMockErrorCallback() {
  MockAudioInputSource source(5, 16000);
  int error_calls = 0;
  std::string received_error;

  auto on_err = [&error_calls, &received_error](std::string_view msg) {
    ++error_calls;
    received_error = std::string(msg);
  };

  TEST_ASSERT(source.Begin("default", nullptr, on_err, nullptr));
  TEST_ASSERT(source.IsRecording());

  source.SimulateError("simulated audio stream failure");
  TEST_ASSERT(error_calls == 1);
  TEST_ASSERT(received_error == "simulated audio stream failure");
  TEST_ASSERT(!source.IsRecording());
}

void TestMockBeginFailure() {
  MockAudioInputSource source(5, 16000);
  source.SetFailOnBegin(true, "custom permission denied");

  std::string error;
  TEST_ASSERT(!source.Begin("default", nullptr, nullptr, &error));
  TEST_ASSERT(error == "custom permission denied");
  TEST_ASSERT(!source.IsRecording());
}

void TestMockAbort() {
  MockAudioInputSource source(5, 16000);
  TEST_ASSERT(source.Begin("default", nullptr, nullptr, nullptr));

  const std::array<int16_t, 480> chunk{};
  source.PushSamples(chunk);

  source.Abort();
  TEST_ASSERT(!source.IsRecording());

  const auto leftover = source.StopAndDrain();
  TEST_ASSERT(leftover.empty());
}

void TestMockDropSamples() {
  // Small capacity: 1 second at 100 samples/sec = 100 samples
  MockAudioInputSource source(1, 100);
  TEST_ASSERT(source.Begin("default", nullptr, nullptr, nullptr));

  const std::vector<int16_t> overflow(200, 1);
  source.PushSamples(overflow);

  TEST_ASSERT(source.DroppedSamples() > 0);
  source.ResetDropCounters();
  TEST_ASSERT(source.DroppedSamples() == 0);

  source.StopAndDrain();
}

void TestAudioCaptureHeadlessLifecycle() {
  const ScopedEnvVar pulse_env("PULSE_SERVER", "unix:/nonexistent/vinput-pulse-test-sock");

  AudioCapture capture;
  TEST_ASSERT(!capture.IsRecording());
  TEST_ASSERT(capture.DroppedSamples() == 0);
  TEST_ASSERT(!capture.FirstBufferLatencyMs().has_value());

  std::string uninit_error;
  TEST_ASSERT(!capture.Begin("default", nullptr, nullptr, &uninit_error));
  TEST_ASSERT(!uninit_error.empty());
  TEST_ASSERT(!capture.IsRecording());

  std::string start_error;
  TEST_ASSERT(capture.Start(&start_error));
  TEST_ASSERT(start_error.empty());
  TEST_ASSERT(!capture.IsRecording());

  std::string connect_error;
  TEST_ASSERT(!capture.Begin("default", nullptr, nullptr, &connect_error));
  TEST_ASSERT(!connect_error.empty());
  TEST_ASSERT(connect_error.find("pipewire-pulse") != std::string::npos);
  TEST_ASSERT(!capture.IsRecording());
  TEST_ASSERT(capture.StopAndDrain().empty());
}

void TestMockNotifyFd() {
  const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  TEST_ASSERT(efd >= 0);

  MockAudioInputSource source(5, 16000);
  source.SetNotifyFd(efd);
  TEST_ASSERT(source.Begin("default", nullptr, nullptr, nullptr));

  const std::array<int16_t, 160> chunk{};
  source.PushSamples(chunk);

  uint64_t count = 0;
  TEST_ASSERT(read(efd, &count, sizeof(count)) == sizeof(count));
  TEST_ASSERT(count >= 1);

  source.SimulateError("err");
  count = 0;
  TEST_ASSERT(read(efd, &count, sizeof(count)) == sizeof(count));
  TEST_ASSERT(count >= 1);

  source.SetNotifyFd(-1);
  close(efd);
}

} // namespace

int main() {
  std::printf("Running test_audio_input_source...\n");
  TestMockLifecycle();
  TestMockFirstBufferCallback();
  TestMockSimulatedLatency();
  TestMockErrorCallback();
  TestMockBeginFailure();
  TestMockAbort();
  TestMockDropSamples();
  TestAudioCaptureHeadlessLifecycle();
  TestMockNotifyFd();
  std::printf("All test_audio_input_source passed!\n");
  return 0;
}
