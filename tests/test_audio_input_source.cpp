#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "daemon/audio/mock_audio_input_source.h"
#include "daemon/audio/pipewire_control_plane.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

using vinput::daemon::audio::MockAudioInputSource;
using vinput::daemon::audio::PipeWireControlPlane;

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

  source.SimulateError("simulated pipewire node failure");
  TEST_ASSERT(error_calls == 1);
  TEST_ASSERT(received_error == "simulated pipewire node failure");
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

void TestPipeWireControlPlaneHeadless() {
  PipeWireControlPlane cp;
  TEST_ASSERT(!cp.Connect(nullptr));
  TEST_ASSERT(!cp.EnsureConnected(nullptr));
  TEST_ASSERT(cp.Core() == nullptr);

  const auto target_empty = cp.ResolveTarget("");
  TEST_ASSERT(target_empty.node_name.empty());
  TEST_ASSERT(target_empty.object_serial.empty());
  TEST_ASSERT(!target_empty.is_sink_capture);

  const auto target_default = cp.ResolveTarget("default");
  TEST_ASSERT(target_default.node_name.empty());
  TEST_ASSERT(target_default.object_serial.empty());
  TEST_ASSERT(!target_default.is_sink_capture);

  cp.Disconnect();
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
  TestPipeWireControlPlaneHeadless();
  std::printf("All test_audio_input_source passed!\n");
  return 0;
}
