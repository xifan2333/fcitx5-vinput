#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace vinput::audio {

inline constexpr size_t kFourCcSize = 4;
inline constexpr size_t kChunkHeaderSize = 8;
inline constexpr size_t kRiffHeaderSize = 12;
inline constexpr size_t kStandardWavHeaderSize = 44;

inline size_t DetectWavDataOffset(std::span<const uint8_t> buffer) {
  if (buffer.size() < kStandardWavHeaderSize) {
    return 0;
  }
  if (std::memcmp(buffer.data(), "RIFF", kFourCcSize) != 0 ||
      std::memcmp(buffer.data() + kChunkHeaderSize, "WAVE", kFourCcSize) != 0) {
    return 0;
  }

  size_t pos = kRiffHeaderSize;
  while (pos + kChunkHeaderSize <= buffer.size()) {
    if (std::memcmp(buffer.data() + pos, "data", kFourCcSize) == 0) {
      return pos + kChunkHeaderSize;
    }
    uint32_t chunk_size = 0;
    std::memcpy(&chunk_size, buffer.data() + pos + kFourCcSize, sizeof(chunk_size));
    const size_t padded_size = (static_cast<size_t>(chunk_size) + 1U) & ~1U;
    if (pos + kChunkHeaderSize + padded_size < pos) {
      break;
    }
    pos += kChunkHeaderSize + padded_size;
  }
  return kStandardWavHeaderSize;
}

} // namespace vinput::audio
