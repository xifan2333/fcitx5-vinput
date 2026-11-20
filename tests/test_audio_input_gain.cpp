#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

#include "daemon/audio/audio_utils.h"

namespace {

void Check(bool ok, const char* message) {
  if (!ok) {
    std::cerr << "Audio input-gain regression failed: " << message << '\n';
    std::abort();
  }
}

void CheckSamples(const std::vector<int16_t>& actual, std::span<const int16_t> expected) {
  Check(actual.size() == expected.size(), "unexpected sample count");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    Check(actual.at(index) == *(expected.begin() + static_cast<std::ptrdiff_t>(index)),
          "unexpected sample value");
  }
}

void TestBufferedCaptureGain() {
  // Buffered mode exports the raw AudioCapture buffer at Stop.
  std::vector<int16_t> captured{1000, -1000, 20000, -20000, 32767, -32768, 0};
  vinput::audio::ApplyGainI16(captured, 2.0F);
  constexpr std::array<int16_t, 7> expected{2000, -2000, 32767, -32768, 32767, -32768, 0};
  CheckSamples(captured, expected);
}

void TestChunkedOnlyGainsRecoveredRawTail() {
  // Previously delivered chunks already have gain. Only append a processed
  // tail of captured raw samples; never amplify pending samples a second time.
  std::vector<int16_t> raw_capture{1000, -1000, 3000, -3000};
  std::vector<int16_t> pending{2000, -2000}; // Already gained.
  constexpr std::size_t delivered_samples = 0;
  const std::size_t tail_start = delivered_samples + pending.size();
  vinput::audio::ApplyGainI16(std::span<int16_t>(raw_capture).subspan(tail_start), 2.0F);
  pending.insert(pending.end(), raw_capture.begin() + static_cast<std::ptrdiff_t>(tail_start),
                 raw_capture.end());
  constexpr std::array<int16_t, 4> expected{2000, -2000, 6000, -6000};
  CheckSamples(pending, expected);
}

void TestUnityAndFractionalGain() {
  std::vector<int16_t> samples{1000, -1000, 32767, -32768};
  vinput::audio::ApplyGainI16(samples, 1.0F);
  constexpr std::array<int16_t, 4> unchanged{1000, -1000, 32767, -32768};
  CheckSamples(samples, unchanged);
  vinput::audio::ApplyGainI16(samples, 0.5F);
  constexpr std::array<int16_t, 4> halved{500, -500, 16383, -16384};
  CheckSamples(samples, halved);
}

void TestSubspanLeavesAlreadyProcessedSamplesAlone() {
  std::vector<int16_t> samples{1000, -1000, 250, -250};
  vinput::audio::ApplyGainI16(std::span<int16_t>(samples).subspan(2), 2.0F);
  constexpr std::array<int16_t, 4> expected{1000, -1000, 500, -500};
  CheckSamples(samples, expected);
  vinput::audio::ApplyGainI16(std::span<int16_t>(samples).subspan(samples.size()), 2.0F);
  CheckSamples(samples, expected);
}

void TestHasNonSilentAudio() {
  const std::array<int16_t, 4> empty_or_zero{0, 0, 0, 0};
  Check(!vinput::audio::HasNonSilentAudio(std::span<const int16_t>{}), "empty must be silent");
  Check(!vinput::audio::HasNonSilentAudio(empty_or_zero), "zeros must be silent");

  const std::array<int16_t, 4> quiet{128, -128, 128, -128};
  Check(!vinput::audio::HasNonSilentAudio(quiet, 1.0F), "128 at unity gain is below threshold");
  Check(vinput::audio::HasNonSilentAudio(quiet, 2.0F), "128 at 2x gain exceeds threshold");
}

} // namespace

int main() {
  TestBufferedCaptureGain();
  TestChunkedOnlyGainsRecoveredRawTail();
  TestUnityAndFractionalGain();
  TestSubspanLeavesAlreadyProcessedSamplesAlone();
  TestHasNonSilentAudio();
  std::cout << "Audio input gain tests passed\n";
}
