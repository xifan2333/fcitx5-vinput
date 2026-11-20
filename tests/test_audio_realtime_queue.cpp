#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "daemon/audio/audio_ring_buffer.h"

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestRingBufferProperties() {
  using vinput::daemon::audio::AudioSampleRingBuffer;

  const AudioSampleRingBuffer ring(15, 16000);
  Check(ring.Capacity() == 240000, "15s @ 16kHz should be 240,000 samples");
  Check(ring.MemoryBytes() == 480000, "240,000 samples should be 480,000 bytes (< 500 KB)");
  Check(ring.Available() == 0, "initial available should be 0");
}

void RunProducer(vinput::daemon::audio::AudioSampleRingBuffer& ring, std::size_t total_chunks,
                 std::size_t samples_per_chunk, std::atomic<bool>& producer_done) {
  std::vector<int16_t> chunk(samples_per_chunk);
  for (std::size_t c = 0; c < total_chunks; ++c) {
    for (std::size_t s = 0; s < samples_per_chunk; ++s) {
      chunk[s] = static_cast<int16_t>((c * samples_per_chunk + s) % 32767);
    }

    std::size_t written = 0;
    while (written < samples_per_chunk) {
      written +=
          ring.Write(std::span<const int16_t>(chunk.data() + written, samples_per_chunk - written));
      if (written < samples_per_chunk) {
        std::this_thread::yield();
      }
    }
  }
  producer_done.store(true, std::memory_order_release);
}

void RunConsumer(vinput::daemon::audio::AudioSampleRingBuffer& ring,
                 std::atomic<bool>& producer_done, std::vector<int16_t>& consumed_samples) {
  while (!producer_done.load(std::memory_order_acquire) || ring.Available() > 0) {
    ring.DrainAvailable([&](std::span<const int16_t> chunk) {
      consumed_samples.insert(consumed_samples.end(), chunk.begin(), chunk.end());
    });
    std::this_thread::yield();
  }
}

void TestSpscConcurrentThroughput() {
  using vinput::daemon::audio::AudioSampleRingBuffer;

  AudioSampleRingBuffer ring(15, 16000);

  constexpr std::size_t kTotalChunks = 2000;
  constexpr std::size_t kSamplesPerChunk = 480; // 30 ms at 16 kHz
  constexpr std::size_t kTotalSamples = kTotalChunks * kSamplesPerChunk;

  std::atomic<bool> producer_done{false};
  std::vector<int16_t> consumed_samples;
  consumed_samples.reserve(kTotalSamples);

  std::thread consumer([&] { RunConsumer(ring, producer_done, consumed_samples); });
  std::thread producer([&] { RunProducer(ring, kTotalChunks, kSamplesPerChunk, producer_done); });

  producer.join();
  consumer.join();

  Check(consumed_samples.size() == kTotalSamples, "all samples should be received without loss");
  for (std::size_t i = 0; i < consumed_samples.size(); ++i) {
    Check(consumed_samples.at(i) == static_cast<int16_t>(i % 32767),
          "sample ordering corrupted in SPSC queue");
  }
}

void TestRingBufferOverflowAndDropTelemetry() {
  using vinput::daemon::audio::AudioSampleRingBuffer;

  AudioSampleRingBuffer ring(1, 1000); // 1000 samples capacity
  Check(ring.Capacity() == 1000, "capacity should be 1000");

  std::vector<int16_t> chunk_800(800, 1);
  const std::size_t written_1 = ring.Write(chunk_800);
  Check(written_1 == 800, "first write should take 800 samples");
  Check(ring.Available() == 800, "800 samples should be available");

  std::vector<int16_t> chunk_400(400, 2);
  const std::size_t written_2 = ring.Write(chunk_400);
  Check(written_2 == 200, "partial write should only accept remaining 200 samples");
  Check(ring.Available() == 1000, "buffer should be fully saturated at 1000 samples");

  const std::size_t written_3 = ring.Write(chunk_400);
  Check(written_3 == 0, "write on saturated buffer should write 0 samples");

  const auto drained = ring.DrainAll();
  Check(drained.size() == 1000, "drained count should match capacity");
  Check(ring.Available() == 0, "buffer should be empty after DrainAll");

  const std::size_t written_4 = ring.Write(chunk_400);
  Check(written_4 == 400, "buffer should accept new items after draining");
}

void TestAtomicLatencyCalculation() {
  std::atomic<int64_t> armed_ns{0};
  std::atomic<int64_t> first_ns{0};

  const auto calc_latency = [&]() -> std::optional<long> {
    const int64_t armed = armed_ns.load(std::memory_order_acquire);
    const int64_t first = first_ns.load(std::memory_order_acquire);
    if (armed <= 0 || first <= 0 || first < armed) {
      return std::nullopt;
    }
    return static_cast<long>((first - armed) / 1'000'000L);
  };

  Check(!calc_latency().has_value(), "unarmed latency should be nullopt");

  armed_ns.store(1'000'000'000L, std::memory_order_release);
  Check(!calc_latency().has_value(), "armed but no buffer should be nullopt");

  first_ns.store(1'050'000'000L, std::memory_order_release);
  const auto latency = calc_latency();
  Check(latency.has_value() && *latency == 50, "latency should be 50 ms");

  // Reset
  armed_ns.store(0, std::memory_order_release);
  first_ns.store(0, std::memory_order_release);
  Check(!calc_latency().has_value(), "reset latency should be nullopt");
}

} // namespace

int main() {
  TestRingBufferProperties();
  TestSpscConcurrentThroughput();
  TestRingBufferOverflowAndDropTelemetry();
  TestAtomicLatencyCalculation();
  std::cout << "All audio real-time queue tests passed successfully!\n";
  return 0;
}
