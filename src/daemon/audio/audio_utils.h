#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vinput::audio {

// Peak-normalize samples in-place. Only amplifies (never attenuates).
// If the peak amplitude is below `low_threshold`, scales so peak == `target_peak`.
void PeakNormalize(std::vector<float>& samples, float target_peak = 1.0F,
                   float low_threshold = 0.1F);

// Apply a fixed input gain to int16 samples in-place. Samples are clamped to [-32768, 32767].
void ApplyGainI16(std::span<int16_t> samples, float gain);
void ApplyGainI16(std::vector<int16_t>& samples, float gain);

// At unity gain (or for empty input) returns `pcm` itself with no copy.
// Otherwise copies into `scratch`, applies gain, and returns a span of it.
std::span<const int16_t> PrepareGainAdjustedInput(std::span<const int16_t> pcm, float gain,
                                                  std::vector<int16_t>* scratch);

// Returns true if `pcm` (optionally scaled by `gain`) reaches speech-level peak or RMS energy.
[[nodiscard]] bool HasNonSilentAudio(std::span<const int16_t> pcm, float gain = 1.0F);

} // namespace vinput::audio
