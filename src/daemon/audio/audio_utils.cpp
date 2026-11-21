#include "audio_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace vinput::audio {

void PeakNormalize(std::vector<float>& samples, float target_peak, float low_threshold) {
  if (samples.empty()) {
    return;
  }

  float peak = 0.0F;
  for (const float s : samples) {
    peak = std::max(peak, std::fabs(s));
  }

  if (peak < 1e-8F || peak >= low_threshold) {
    return;
  }

  const float scale = target_peak / peak;
  for (float& s : samples) {
    s *= scale;
  }
}

void ApplyGainI16(std::span<int16_t> samples, float gain) {
  if (samples.empty() || gain == 1.0F) {
    return;
  }

  for (int16_t& s : samples) {
    const float scaled = static_cast<float>(s) * gain;
    s = static_cast<int16_t>(std::clamp(scaled, -32768.0F, 32767.0F));
  }
}

void ApplyGainI16(std::vector<int16_t>& samples, float gain) {
  ApplyGainI16(std::span<int16_t>(samples), gain);
}

std::span<const int16_t> PrepareGainAdjustedInput(std::span<const int16_t> pcm, float gain,
                                                  std::vector<int16_t>* scratch) {
  if (gain == 1.0F || pcm.empty()) {
    return pcm;
  }
  scratch->assign(pcm.begin(), pcm.end());
  ApplyGainI16(*scratch, gain);
  return *scratch;
}

bool HasNonSilentAudio(std::span<const int16_t> pcm, float gain) {
  if (pcm.empty()) {
    return false;
  }

  constexpr float kNonSilentPeakThreshold = 0.02F;
  constexpr float kNonSilentRmsThreshold = 0.005F;

  double sum_squares = 0.0;
  float peak = 0.0F;
  for (const int16_t sample : pcm) {
    const float scaled = (gain == 1.0F)
                             ? static_cast<float>(sample)
                             : std::clamp(static_cast<float>(sample) * gain, -32768.0F, 32767.0F);
    const float value = scaled / 32768.0F;
    const float abs_value = std::fabs(value);
    peak = std::max(peak, abs_value);
    sum_squares += static_cast<double>(value) * static_cast<double>(value);
  }

  const auto rms = static_cast<float>(std::sqrt(sum_squares / static_cast<double>(pcm.size())));
  return peak >= kNonSilentPeakThreshold || rms >= kNonSilentRmsThreshold;
}

} // namespace vinput::audio
