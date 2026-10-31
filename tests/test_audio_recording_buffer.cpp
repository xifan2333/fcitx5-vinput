#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <vector>

#include "daemon/runtime/audio_recording_buffer.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << "\n";
    std::abort();
  }
}

void ExpectSamples(const std::vector<int16_t>& got, const std::vector<int16_t>& expected) {
  if (got != expected) {
    std::cerr << "Sample mismatch: got size=" << got.size() << " expected size=" << expected.size()
              << "\n";
    std::abort();
  }
}

void TestStartupBufferingStreamingMode() {
  vinput::daemon::runtime::AudioRecordingBuffer buffer;
  buffer.Reset(1.0F);

  const std::array<int16_t, 3> startup_a{10, 20, 30};
  const std::array<int16_t, 2> startup_b{40, 50};
  buffer.AppendStartup(startup_a);
  buffer.AppendStartup(startup_b);
  Expect(buffer.TotalSampleCount() == 5, "TotalSampleCount should be 5 after startup");

  // Activate in streaming mode
  buffer.Activate(/*is_buffered=*/false);

  // Generate 1595 samples so combined total is exactly 1600 (1 chunk)
  std::vector<int16_t> live(1595);
  std::iota(live.begin(), live.end(), static_cast<int16_t>(100));
  buffer.Append(live);
  Expect(buffer.TotalSampleCount() == 1600, "TotalSampleCount should be 1600");

  auto chunk = buffer.PopStreamingChunk(1600);
  Expect(chunk.has_value(), "Expected a chunk of 1600 samples");
  Expect(chunk->size() == 1600, "Chunk size should be 1600");
  Expect((*chunk)[0] == 10 && (*chunk)[1] == 20 && (*chunk)[4] == 50,
         "Startup samples should be at beginning of chunk");
  Expect((*chunk)[5] == 100, "Live samples should follow startup samples");

  // No further complete chunks
  Expect(!buffer.PopStreamingChunk(1600).has_value(), "Should be no more chunks");
  Expect(buffer.DrainStreamingTail().empty(), "Tail should be empty");
  Expect(buffer.TotalSampleCount() == 1600, "Total count preserved after pop");
}

void TestGainApplicationAndBufferedMode() {
  vinput::daemon::runtime::AudioRecordingBuffer buffer;
  buffer.Reset(2.0F);

  const std::array<int16_t, 3> startup{1000, -1000, 20000};
  buffer.AppendStartup(startup);

  // Activate in buffered mode: startup samples should have gain applied
  buffer.Activate(/*is_buffered=*/true);

  const std::array<int16_t, 2> live{500, -500};
  buffer.Append(live);

  Expect(buffer.TotalSampleCount() == 5, "Total sample count should be 5");

  auto full_pcm = buffer.TakeBufferedPcm();
  ExpectSamples(full_pcm, {2000, -2000, 32767, 1000, -1000});
  Expect(buffer.TakeBufferedPcm().empty(), "Second TakeBufferedPcm should be empty");
  Expect(buffer.TotalSampleCount() == 5, "Total count preserved after TakeBufferedPcm");
}

void TestStreamingChunkingAndTail() {
  vinput::daemon::runtime::AudioRecordingBuffer buffer;
  buffer.Reset(1.0F);
  buffer.Activate(/*is_buffered=*/false);

  // Append 3500 samples: should yield two 1600 chunks and a 300 sample tail
  std::vector<int16_t> data(3500, 42);
  buffer.Append(data);
  Expect(buffer.TotalSampleCount() == 3500, "Total count should be 3500");

  auto chunk1 = buffer.PopStreamingChunk(1600);
  Expect(chunk1.has_value() && chunk1->size() == 1600, "Chunk 1 should have 1600 samples");

  auto chunk2 = buffer.PopStreamingChunk(1600);
  Expect(chunk2.has_value() && chunk2->size() == 1600, "Chunk 2 should have 1600 samples");

  auto chunk3 = buffer.PopStreamingChunk(1600);
  Expect(!chunk3.has_value(), "Chunk 3 should not be available (only 300 remain)");

  auto tail = buffer.DrainStreamingTail();
  Expect(tail.size() == 300, "Tail should have 300 samples");

  auto empty_tail = buffer.DrainStreamingTail();
  Expect(empty_tail.empty(), "Subsequent tail should be empty");
  Expect(buffer.TotalSampleCount() == 3500, "Total sample count preserved after drain");
}

void TestAppendBeforeActivate() {
  vinput::daemon::runtime::AudioRecordingBuffer buffer;
  buffer.Reset(1.0F);

  // Append before Activate should be treated as pre-roll startup
  const std::array<int16_t, 3> samples{1, 2, 3};
  buffer.Append(samples);
  Expect(buffer.TotalSampleCount() == 3, "Total count should be 3");

  buffer.Activate(/*is_buffered=*/true);
  auto pcm = buffer.TakeBufferedPcm();
  ExpectSamples(pcm, {1, 2, 3});
}

void TestSetGainAndClear() {
  vinput::daemon::runtime::AudioRecordingBuffer buffer;
  buffer.Reset(1.0F);
  Expect(buffer.Gain() == 1.0F, "Default gain should be 1.0F");

  buffer.AppendStartup(std::array<int16_t, 1>{1000});
  buffer.SetGain(3.0F);
  Expect(buffer.Gain() == 3.0F, "Gain should be 3.0F");

  buffer.Activate(/*is_buffered=*/true);
  ExpectSamples(buffer.TakeBufferedPcm(), {3000});

  // Clear resets everything
  buffer.Clear();
  Expect(buffer.TotalSampleCount() == 0, "Clear should reset TotalSampleCount");
  Expect(buffer.Gain() == 1.0F, "Clear should reset gain");
  Expect(buffer.TakeBufferedPcm().empty(), "Clear should empty buffered pcm");
  Expect(buffer.DrainStreamingTail().empty(), "Clear should empty streaming pcm");
}

} // namespace

int main() {
  TestStartupBufferingStreamingMode();
  TestGainApplicationAndBufferedMode();
  TestStreamingChunkingAndTail();
  TestAppendBeforeActivate();
  TestSetGainAndClear();
  std::cout << "All AudioRecordingBuffer tests passed!\n";
  return 0;
}
