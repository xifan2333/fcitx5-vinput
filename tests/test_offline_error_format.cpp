#include <cstdlib>
#include <iostream>
#include <string>

#include "common/asr/model_manager.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestReshapeErrorFormatted() {
  const std::string raw =
      "Non-zero status code returned while running Reshape node. "
      "'/encoder/0/layers.0/self_attn_weights/Reshape_3' Input shape: {1,656,16} Requested shape: "
      "{-1,5311,4,4} input tensor cannot be reshaped";
  const auto formatted = FormatOfflineAsrError(raw, 53.2);
  Expect(formatted.find("53.2") != std::string::npos, "Formatted error must contain duration");
  Expect(formatted.find("Reshape") == std::string::npos,
         "Formatted error must not show raw Reshape trace to user");
  Expect(!formatted.empty(), "Formatted error must not be empty");
}

void TestOtherErrorPreserved() {
  const std::string raw = "model file corrupted";
  const auto formatted = FormatOfflineAsrError(raw, 5.0);
  Expect(formatted == raw, "Unrelated errors must be preserved");
}

} // namespace

int main() {
  TestReshapeErrorFormatted();
  TestOtherErrorPreserved();

  std::cout << "All offline error formatting tests passed!\n";
  return 0;
}
