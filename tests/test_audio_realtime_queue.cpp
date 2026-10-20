#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <readerwriterqueue.h>
#include <thread>
#include <type_traits>
#include <vector>

#include "daemon/audio/audio_types.h"

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestAudioPcmChunkProperties() {
  static_assert(std::is_trivially_copyable_v<vinput::audio::AudioPcmChunk>);
  static_assert(sizeof(vinput::audio::AudioPcmChunk) >=
                vinput::audio::kMaxChunkSamples * sizeof(int16_t));

  vinput::audio::AudioPcmChunk chunk;
  Check(chunk.empty(), "chunk should be empty initially");
  Check(chunk.size == 0, "chunk size should be 0");

  std::array<int16_t, 100> src{};
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = static_cast<int16_t>(i + 1);
  }

  auto created = vinput::audio::AudioPcmChunk::FromSpan(src);
  Check(!created.empty(), "created chunk should not be empty");
  Check(created.size == 100, "created chunk size should be 100");
  Check(created.AsSpan().size() == 100, "span size should be 100");
  for (std::size_t i = 0; i < 100; ++i) {
    Check(created.samples[i] == static_cast<int16_t>(i + 1), "sample value mismatch");
  }

  // Truncation at kMaxChunkSamples
  std::vector<int16_t> oversized(vinput::audio::kMaxChunkSamples + 500, 42);
  auto clamped = vinput::audio::AudioPcmChunk::FromSpan(oversized);
  Check(clamped.size == vinput::audio::kMaxChunkSamples, "chunk size should be clamped to max");

  clamped.clear();
  Check(clamped.empty(), "cleared chunk should be empty");
}

void TestSpscConcurrentThroughput() {
  constexpr std::size_t kQueueCapacity = 256;
  moodycamel::ReaderWriterQueue<vinput::audio::AudioPcmChunk> queue(kQueueCapacity);

  constexpr std::size_t kTotalChunks = 5000;
  constexpr std::size_t kSamplesPerChunk = 320; // 20 ms at 16 kHz

  std::atomic<bool> producer_done{false};
  std::vector<int16_t> consumed_samples;
  consumed_samples.reserve(kTotalChunks * kSamplesPerChunk);

  std::thread consumer([&]() {
    vinput::audio::AudioPcmChunk chunk;
    while (!producer_done.load(std::memory_order_acquire) || queue.size_approx() > 0) {
      if (queue.try_dequeue(chunk)) {
        consumed_samples.insert(consumed_samples.end(), chunk.samples.begin(),
                                chunk.samples.begin() + chunk.size);
      } else {
        std::this_thread::yield();
      }
    }
    // Drain any remaining
    while (queue.try_dequeue(chunk)) {
      consumed_samples.insert(consumed_samples.end(), chunk.samples.begin(),
                              chunk.samples.begin() + chunk.size);
    }
  });

  std::thread producer([&]() {
    for (std::size_t c = 0; c < kTotalChunks; ++c) {
      vinput::audio::AudioPcmChunk chunk;
      chunk.size = kSamplesPerChunk;
      for (std::size_t s = 0; s < kSamplesPerChunk; ++s) {
        chunk.samples[s] = static_cast<int16_t>((c * kSamplesPerChunk + s) % 32767);
      }

      while (!queue.try_enqueue(chunk)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();

  Check(consumed_samples.size() == kTotalChunks * kSamplesPerChunk,
        "all samples should be received without loss");
  for (std::size_t i = 0; i < consumed_samples.size(); ++i) {
    Check(consumed_samples[i] == static_cast<int16_t>(i % 32767),
          "sample ordering corrupted in SPSC queue");
  }
}

void TestQueueOverflowAndDropTelemetry() {
  constexpr std::size_t kCapacity = 4;
  moodycamel::ReaderWriterQueue<vinput::audio::AudioPcmChunk> queue(kCapacity);

  std::atomic<std::size_t> dropped_chunks{0};
  std::atomic<std::size_t> dropped_samples{0};

  vinput::audio::AudioPcmChunk chunk;
  chunk.size = 256;

  std::size_t enqueued_count = 0;
  for (std::size_t i = 0; i < 10; ++i) {
    if (queue.try_enqueue(chunk)) {
      ++enqueued_count;
    } else {
      dropped_chunks.fetch_add(1, std::memory_order_relaxed);
      dropped_samples.fetch_add(chunk.size, std::memory_order_relaxed);
    }
  }

  Check(enqueued_count <= kCapacity + 2, "queue should reject items when full");
  Check(dropped_chunks.load() > 0, "dropped chunks counter must record overflows");
  Check(dropped_samples.load() == dropped_chunks.load() * 256,
        "dropped samples count must match dropped chunks * size");

  // Draining restores space
  vinput::audio::AudioPcmChunk drained;
  std::size_t drained_count = 0;
  while (queue.try_dequeue(drained)) {
    ++drained_count;
  }
  Check(drained_count == enqueued_count, "drained count should match enqueued");

  // Queue can now accept new items
  Check(queue.try_enqueue(chunk), "queue should accept items after being drained");
}

} // namespace

int main() {
  TestAudioPcmChunkProperties();
  TestSpscConcurrentThroughput();
  TestQueueOverflowAndDropTelemetry();
  std::cout << "All audio real-time queue tests passed successfully!\n";
  return 0;
}
