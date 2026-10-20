#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace vinput::audio {

// Maximum samples stored in a single chunk (2048 samples = 128 ms at 16 kHz).
// Zero-heap-allocation value type designed for wait-free real-time audio queues.
constexpr std::size_t kMaxChunkSamples = 2048;

struct AudioPcmChunk {
  std::array<int16_t, kMaxChunkSamples> samples{};
  std::size_t size = 0;

  constexpr AudioPcmChunk() noexcept = default;

  static AudioPcmChunk FromSpan(std::span<const int16_t> src) noexcept {
    AudioPcmChunk chunk;
    const std::size_t copy_size = std::min(src.size(), kMaxChunkSamples);
    if (copy_size > 0) {
      std::copy_n(src.data(), copy_size, chunk.samples.data());
      chunk.size = copy_size;
    }
    return chunk;
  }

  [[nodiscard]] std::span<const int16_t> AsSpan() const noexcept { return {samples.data(), size}; }

  [[nodiscard]] std::span<int16_t> AsSpan() noexcept { return {samples.data(), size}; }

  [[nodiscard]] bool empty() const noexcept { return size == 0; }

  void clear() noexcept { size = 0; }
};

static_assert(std::is_trivially_copyable_v<AudioPcmChunk>,
              "AudioPcmChunk must be trivially copyable for real-time ringbuffers");

} // namespace vinput::audio
