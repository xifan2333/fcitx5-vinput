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
      "'/encoder/0/layers.0/self_attn_weights/Reshape_3' Input shape: {1,656,16}";
  const auto formatted = FormatOfflineAsrError(raw, 5.0);
  Expect(formatted.find("5.0") != std::string::npos, "Formatted error must contain duration");
  Expect(formatted.find("Reshape") == std::string::npos,
         "Formatted error must not show raw Reshape trace to user");
  Expect(!formatted.empty(), "Formatted error must not be empty");
}

void TestCannotBeReshapedErrorFormatted() {
  const std::string raw = "input tensor cannot be reshaped";
  const auto formatted = FormatOfflineAsrError(raw, 5.0);
  Expect(formatted.find("5.0") != std::string::npos, "Formatted error must contain duration");
  Expect(formatted.find("cannot be reshaped") == std::string::npos,
         "Formatted error must not show raw cannot be reshaped text to user");
}

void TestOtherErrorPreservedShort() {
  const std::string raw = "model file corrupted";
  const auto formatted = FormatOfflineAsrError(raw, 5.0);
  Expect(formatted == raw, "Unrelated errors on short audio must be preserved");
}

void TestOtherErrorPreservedLong() {
  const std::string raw = "model file corrupted";
  const auto formatted = FormatOfflineAsrError(raw, 50.0);
  Expect(formatted == raw,
         "Unrelated errors on long audio must not be falsely masked as duration error");
}

} // namespace

int main() {
  TestReshapeErrorFormatted();
  TestCannotBeReshapedErrorFormatted();
  TestOtherErrorPreservedShort();
  TestOtherErrorPreservedLong();

  std::cout << "All offline error formatting tests passed!\n";
  return 0;
}
