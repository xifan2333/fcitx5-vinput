#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "common/i18n.h"
#include "common/utils/string_utils.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

std::string FormatLongAudioError(std::string_view raw_error, double duration_sec) {
  if (raw_error.find("Reshape") != std::string_view::npos ||
      raw_error.find("cannot be reshaped") != std::string_view::npos || duration_sec > 25.0) {
    return vinput::str::FmtStr(
        _("Recognition failed: audio is about %.1f seconds long, which exceeds what this offline "
          "model can reliably process. Please record in shorter segments or switch to a "
          "streaming model."),
        duration_sec);
  }
  return std::string(raw_error);
}

void TestReshapeErrorFormatted() {
  const std::string raw =
      "Non-zero status code returned while running Reshape node. "
      "'/encoder/0/layers.0/self_attn_weights/Reshape_3' Input shape: {1,656,16} Requested shape: "
      "{-1,5311,4,4} input tensor cannot be reshaped";
  const auto formatted = FormatLongAudioError(raw, 53.2);
  Expect(formatted.find("53.2") != std::string::npos, "Formatted error must contain duration");
  Expect(formatted.find("Reshape") == std::string::npos,
         "Formatted error must not show raw Reshape trace to user");
  Expect(!formatted.empty(), "Formatted error must not be empty");
}

void TestOtherErrorPreserved() {
  const std::string raw = "model file corrupted";
  const auto formatted = FormatLongAudioError(raw, 5.0);
  Expect(formatted == raw, "Unrelated errors must be preserved");
}

} // namespace

int main() {
  TestReshapeErrorFormatted();
  TestOtherErrorPreserved();

  std::cout << "All offline error formatting tests passed!\n";
  return 0;
}
