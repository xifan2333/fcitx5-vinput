#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace vinput::audio {

inline constexpr size_t kFourCcSize = 4;
inline constexpr size_t kChunkHeaderSize = 8;
inline constexpr size_t kRiffHeaderSize = 12;
// A `data` chunk declaring this size means "stream until EOF" (RIFF convention);
// the header carries no usable payload boundary.
inline constexpr uint32_t kWavStreamingDataSize = 0xFFFFFFFFU;

// Payload bounds of the `data` chunk inside a scanned RIFF/WAVE buffer.
struct WavDataBounds {
  // Byte offset inside the scanned buffer where the `data` payload begins.
  std::size_t offset = 0;
  // Declared payload size, or kWavStreamingDataSize when unbounded.
  uint32_t size = 0;
};

// True when `buffer` starts with the full 12-byte "RIFF<size>WAVE" signature.
inline bool StartsWithWavPrefix(std::span<const uint8_t> buffer) noexcept {
  return buffer.size() >= kRiffHeaderSize && std::memcmp(buffer.data(), "RIFF", kFourCcSize) == 0 &&
         std::memcmp(buffer.data() + kChunkHeaderSize, "WAVE", kFourCcSize) == 0;
}

enum class WavScanResult : std::uint8_t {
  NeedMoreData, // chunk header, or its padded extent for non-data chunks, is not buffered yet
  Skipped,      // a non-audio chunk is fully present; advance to NextPos
  DataFound,    // the `data` chunk header is present; Bounds filled
  Malformed,    // chunk size would wrap the address space
};

struct WavScanOutcome {
  WavScanResult result = WavScanResult::NeedMoreData;
  std::size_t next_pos = 0;
  WavDataBounds bounds;
};

// Scans the chunk header at `scan_pos` of a RIFF/WAVE buffer (StartsWithWavPrefix
// must hold). For a `data` chunk only the 8-byte header is required; payload bytes
// are delivered incrementally by the caller. Non-data chunks jump to their padded
// extent, which must be fully buffered before advancing.
inline WavScanOutcome ScanWavChunk(std::span<const uint8_t> buffer, std::size_t scan_pos) noexcept {
  WavScanOutcome outcome;
  if (scan_pos + kChunkHeaderSize > buffer.size()) {
    return outcome; // NeedMoreData
  }

  uint32_t chunk_size = 0;
  std::memcpy(&chunk_size, buffer.data() + scan_pos + kFourCcSize, sizeof(chunk_size));

  if (std::memcmp(buffer.data() + scan_pos, "data", kFourCcSize) == 0) {
    outcome.result = WavScanResult::DataFound;
    outcome.bounds.offset = scan_pos + kChunkHeaderSize;
    outcome.bounds.size = chunk_size;
    return outcome;
  }

  const std::size_t padded_size = (static_cast<std::size_t>(chunk_size) + 1U) & ~1U;
  if (chunk_size > std::numeric_limits<std::size_t>::max() - kChunkHeaderSize - scan_pos) {
    outcome.result = WavScanResult::Malformed;
    return outcome;
  }
  const std::size_t chunk_extent = scan_pos + kChunkHeaderSize + padded_size;
  if (chunk_extent > buffer.size()) {
    return outcome; // NeedMoreData
  }
  outcome.result = WavScanResult::Skipped;
  outcome.next_pos = chunk_extent;
  return outcome;
}

// Drives ScanWavChunk over a buffered RIFF/WAVE stream: skips fully-present non-data
// chunks (advancing `scan_pos`) and returns the first DataFound / NeedMoreData /
// Malformed outcome. Callers append more bytes and call again on NeedMoreData.
inline WavScanOutcome WalkWavChunks(std::span<const uint8_t> buffer,
                                    std::size_t& scan_pos) noexcept {
  for (;;) {
    const auto outcome = ScanWavChunk(buffer, scan_pos);
    if (outcome.result == WavScanResult::Skipped) {
      scan_pos = outcome.next_pos;
      continue;
    }
    return outcome;
  }
}

} // namespace vinput::audio
