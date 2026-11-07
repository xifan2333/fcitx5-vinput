#include <cstdlib>
#include <iostream>
#include <string>

#include "common/asr/recognition_result.h"
#include "common/client/client_event.h"

namespace {

void ExpectTrue(bool condition, const std::string& msg = "") {
  if (!condition) {
    std::cerr << "ExpectTrue failed! " << msg << "\n";
    std::abort();
  }
}

void ExpectEq(const std::string& actual, const std::string& expected, const std::string& msg = "") {
  if (actual != expected) {
    std::cerr << "ExpectEq failed: expected '" << expected << "', got '" << actual << "' (" << msg
              << ")\n";
    std::abort();
  }
}

void TestSerializeStatusEvent() {
  const std::string json = vinput::client::SerializeStatusEvent("recording", true);
  ExpectEq(json, R"({"type":"status","status":"recording","ime":true})", "Status true mismatch");

  const std::string json_silent = vinput::client::SerializeStatusEvent("idle", false);
  ExpectEq(json_silent, R"({"type":"status","status":"idle","ime":false})",
           "Status false mismatch");
}

void TestSerializePartialEvent() {
  const std::string json = vinput::client::SerializePartialEvent("testing audio", true);
  ExpectEq(json, R"({"type":"partial","text":"testing audio","ime":true})",
           "Partial true mismatch");

  const std::string json_silent = vinput::client::SerializePartialEvent("bg audio", false);
  ExpectEq(json_silent, R"({"type":"partial","text":"bg audio","ime":false})",
           "Partial false mismatch");
}

void TestSerializeCommittedEvent() {
  vinput::result::Payload payload;
  payload.commitText = "Hello world.";
  payload.candidates = {
      {.text = "Hello world.", .source = "llm"},
      {.text = "Hello world", .source = "raw"},
  };

  const std::string json = vinput::client::SerializeCommittedEvent(payload, true);
  ExpectEq(
      json,
      R"({"type":"committed","text":"Hello world.","candidates":[{"text":"Hello world.","source":"llm"},{"text":"Hello world","source":"raw"}],"ime":true})",
      "Committed true mismatch");

  const std::string json_silent = vinput::client::SerializeCommittedEvent(payload, false);
  ExpectEq(
      json_silent,
      R"({"type":"committed","text":"Hello world.","candidates":[{"text":"Hello world.","source":"llm"},{"text":"Hello world","source":"raw"}],"ime":false})",
      "Committed false mismatch");
}

void TestSanitizeControlCharacters() {
  // 0x01 is not escapable by glaze's table and would otherwise emit NUL bytes,
  // producing invalid JSON for every receiver. It must be stripped at the boundary.
  const std::string json = vinput::client::SerializePartialEvent(std::string("a\x01\x1f\nb"), true);
  ExpectEq(json, R"({"type":"partial","text":"a\nb","ime":true})",
           "Unescapable control characters must be stripped");
}

} // namespace

int main() {
  TestSerializeStatusEvent();
  TestSerializePartialEvent();
  TestSerializeCommittedEvent();
  TestSanitizeControlCharacters();
  std::cout << "All client_event tests passed!\n";
  return 0;
}
