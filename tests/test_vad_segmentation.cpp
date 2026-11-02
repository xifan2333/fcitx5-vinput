#include <cstdlib>
#include <iostream>
#include <vector>

#include "daemon/asr/vad_segmentation.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

// 1. Single segment exactly at max duration (15.0s at 16kHz = 240,000 samples) must NOT be sliced.
void TestExactMaxDurationNotSliced() {
  constexpr int kSampleRate = 16000;
  constexpr std::size_t kSamples15s = 240000;
  const std::vector<std::vector<float>> input = {std::vector<float>(kSamples15s, 0.1F)};

  auto result = CoalesceAndSliceSegments(input, kSampleRate, 15.0F, 0.50F);
  Expect(result.size() == 1, "Segment exactly at 15.0s must not be sliced");
  Expect(result[0].size() == kSamples15s, "Segment size must remain exactly 240,000 samples");
}

// 2. Segment of 15.0s + 1 sample must be sliced into two segments: 15.0s and 1 sample.
void TestMaxDurationPlusOneSampleSliced() {
  constexpr int kSampleRate = 16000;
  constexpr std::size_t kSamples15sPlusOne = 240001;
  const std::vector<std::vector<float>> input = {std::vector<float>(kSamples15sPlusOne, 0.2F)};

  auto result = CoalesceAndSliceSegments(input, kSampleRate, 15.0F, 0.50F);
  Expect(result.size() == 2, "Segment of 15.0s + 1 sample must be sliced into 2 segments");
  Expect(result[0].size() == 240000, "First segment must be exactly 15.0s (240,000 samples)");
  Expect(result[1].size() == 1, "Second segment must be the remaining 1 sample");
}

// 3. Consecutive short fragments (< 0.5s / 8000 samples) must be coalesced into a single segment.
void TestConsecutiveShortFragmentsCoalesced() {
  constexpr int kSampleRate = 16000;
  // Three fragments of 0.2s each (3,200 samples)
  const std::vector<std::vector<float>> input = {
      std::vector<float>(3200, 0.1F),
      std::vector<float>(3200, 0.2F),
      std::vector<float>(3200, 0.3F),
  };

  auto result = CoalesceAndSliceSegments(input, kSampleRate, 15.0F, 0.50F);
  Expect(result.size() == 1, "Three 0.2s fragments must coalesce into 1 segment");
  Expect(result[0].size() == 9600, "Coalesced segment size must be 9,600 samples (0.6s)");
}

// 4. Short fragment followed by a long fragment must NOT coalesce if combined size exceeds max
// duration.
void TestShortFragmentNextToMaxDurationNotExceedingMax() {
  constexpr int kSampleRate = 16000;
  // 0.2s (3,200 samples) + 14.9s (238,400 samples) = 241,600 samples > 240,000 (15.0s)
  const std::vector<std::vector<float>> input = {
      std::vector<float>(3200, 0.1F),
      std::vector<float>(238400, 0.2F),
  };

  auto result = CoalesceAndSliceSegments(input, kSampleRate, 15.0F, 0.50F);
  Expect(result.size() == 2, "Segments must not coalesce when combined size exceeds max duration");
  Expect(result[0].size() == 3200, "First segment remains unchanged");
  Expect(result[1].size() == 238400, "Second segment remains unchanged");
}

// 5. Empty segments must be completely filtered out.
void TestEmptySegmentsFiltered() {
  constexpr int kSampleRate = 16000;
  const std::vector<std::vector<float>> input = {
      {}, std::vector<float>(16000, 0.1F), {}, std::vector<float>(32000, 0.2F), {},
  };

  auto result = CoalesceAndSliceSegments(input, kSampleRate, 15.0F, 0.50F);
  Expect(result.size() == 2, "Empty segments must be filtered out");
  Expect(result[0].size() == 16000, "First valid segment preserved");
  Expect(result[1].size() == 32000, "Second valid segment preserved");
}

// 6. Invalid sample rate or empty input returns input unchanged defensively.
void TestDefensiveParameters() {
  const std::vector<std::vector<float>> input = {std::vector<float>(100, 0.1F)};
  Expect(CoalesceAndSliceSegments(input, 0).size() == 1, "Zero sample rate returns input");
  Expect(CoalesceAndSliceSegments(input, -1).size() == 1, "Negative sample rate returns input");
  Expect(CoalesceAndSliceSegments({}, 16000).empty(), "Empty input returns empty");
}

} // namespace

int main() {
  TestExactMaxDurationNotSliced();
  TestMaxDurationPlusOneSampleSliced();
  TestConsecutiveShortFragmentsCoalesced();
  TestShortFragmentNextToMaxDurationNotExceedingMax();
  TestEmptySegmentsFiltered();
  TestDefensiveParameters();

  std::cout << "All VAD segmentation unit tests passed!\n";
  return 0;
}
