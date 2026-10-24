#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vinput::audio {

// Normalize little-endian signed 16-bit PCM using RMS, ignoring a partial tail.
// A level of 7000 sample units reaches 1.0; louder values are clamped.
inline float NormalizePcmRms(std::span<const std::uint8_t> pcm, float reference = 7000.0F) {
  const std::size_t sample_count = pcm.size() / 2;
  if (sample_count == 0 || reference <= 0.0F) {
    return 0.0F;
  }

  std::uint64_t sum_squares = 0;
  for (std::size_t index = 0; index < sample_count; ++index) {
    const auto low = static_cast<std::uint16_t>(pcm[index * 2]);
    const auto high = static_cast<std::uint16_t>(pcm[index * 2 + 1]);
    const std::uint16_t bits = static_cast<std::uint16_t>(low | (high << 8));
    const std::int32_t sample = bits < 0x8000U ? static_cast<std::int32_t>(bits)
                                               : static_cast<std::int32_t>(bits) - 0x10000;
    sum_squares += static_cast<std::uint64_t>(sample * sample);
  }

  const double rms = std::sqrt(static_cast<double>(sum_squares) / sample_count);
  return std::min(1.0F, static_cast<float>(rms / reference));
}

} // namespace vinput::audio
