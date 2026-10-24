#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "common/audio/pcm_level.h"

namespace {

void Require(bool condition, const char* name) {
  if (!condition) {
    std::cerr << "Recording orb audio-level regression: " << name << '\n';
    std::abort();
  }
}

void TestSilenceAndEmptyInput() {
  constexpr std::array<std::uint8_t, 4> silence{0, 0, 0, 0};
  Require(vinput::audio::NormalizePcmRms({}) == 0.0F, "empty input maps to zero");
  Require(vinput::audio::NormalizePcmRms(silence) == 0.0F, "silence maps to zero");
}

void TestRmsNormalizationAndClamping() {
  constexpr std::array<std::uint8_t, 8> half_level{0xAC, 0x0D, 0x54, 0xF2,
                                                   0xAC, 0x0D, 0x54, 0xF2};
  Require(std::abs(vinput::audio::NormalizePcmRms(half_level) - 0.5F) < 0.001F,
          "RMS normalizes sample magnitude");

  constexpr std::array<std::uint8_t, 4> loud{0xB0, 0x36, 0x50, 0xC9};
  Require(vinput::audio::NormalizePcmRms(loud) == 1.0F, "loud input clamps to one");
}

void TestOddTrailingByteIsIgnored() {
  constexpr std::array<std::uint8_t, 5> odd{0xAC, 0x0D, 0x54, 0xF2, 0xFF};
  Require(std::abs(vinput::audio::NormalizePcmRms(odd) - 0.5F) < 0.001F,
          "incomplete trailing PCM sample is ignored");
}

} // namespace

int main() {
  TestSilenceAndEmptyInput();
  TestRmsNormalizationAndClamping();
  TestOddTrailingByteIsIgnored();
  std::cout << "Recording orb audio-level tests passed\n";
}
