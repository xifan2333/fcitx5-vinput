#include <cstdlib>
#include <iostream>
#include <string>

#include "common/asr/recognition_result.h"
#include "common/client/client_event.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << "\n";
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestStreamWireProtocol() {
  using vinput::client::SerializeCommittedEvent;
  using vinput::client::SerializePartialEvent;
  using vinput::client::SerializeStatusEvent;

  const std::string st = SerializeStatusEvent("transcribing", false);
  TEST_ASSERT(st.find('\n') == std::string::npos);
  TEST_ASSERT(st.find(R"("type":"status")") != std::string::npos);
  TEST_ASSERT(st.find(R"("ime":false)") != std::string::npos);

  const std::string part = SerializePartialEvent("testing", false);
  TEST_ASSERT(part.find('\n') == std::string::npos);
  TEST_ASSERT(part.find(R"("type":"partial")") != std::string::npos);
  TEST_ASSERT(part.find(R"("text":"testing")") != std::string::npos);

  vinput::result::Payload payload;
  payload.commitText = "Recognized words.";
  const std::string comm = SerializeCommittedEvent(payload, true);
  TEST_ASSERT(comm.find('\n') == std::string::npos);
  TEST_ASSERT(comm.find(R"("type":"committed")") != std::string::npos);
  TEST_ASSERT(comm.find(R"("text":"Recognized words.")") != std::string::npos);
  TEST_ASSERT(comm.find(R"("ime":true)") != std::string::npos);
}

} // namespace

int main() {
  TestStreamWireProtocol();
  std::cout << "All stream pipeline tests passed successfully.\n";
  return 0;
}
