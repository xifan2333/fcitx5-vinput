#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/audio/wav_probe.h"
#include "common/client/client_event.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << "\n";
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestDetectWavDataOffsetSmallBuffer() {
  using vinput::audio::DetectWavDataOffset;

  std::vector<uint8_t> empty;
  TEST_ASSERT(DetectWavDataOffset(empty) == 0);

  std::vector<uint8_t> small(20, 0);
  TEST_ASSERT(DetectWavDataOffset(small) == 0);
}

void TestDetectWavDataOffsetRawPcm() {
  using vinput::audio::DetectWavDataOffset;

  std::vector<uint8_t> pcm(100, 0x55);
  TEST_ASSERT(DetectWavDataOffset(pcm) == 0);
}

void TestDetectWavDataOffsetStandardWav() {
  using vinput::audio::DetectWavDataOffset;

  std::vector<uint8_t> wav(44 + 100, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  const uint32_t file_size_minus_8 = 44 + 100 - 8;
  std::memcpy(wav.data() + 4, &file_size_minus_8, 4);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "fmt ", 4);
  const uint32_t fmt_size = 16;
  std::memcpy(wav.data() + 16, &fmt_size, 4);
  std::memcpy(wav.data() + 36, "data", 4);
  const uint32_t data_size = 100;
  std::memcpy(wav.data() + 40, &data_size, 4);

  TEST_ASSERT(DetectWavDataOffset(wav) == 44);
}

void TestDetectWavDataOffsetWithPrecedingChunk() {
  using vinput::audio::DetectWavDataOffset;

  std::vector<uint8_t> wav(56 + 100, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "fmt ", 4);
  const uint32_t fmt_size = 16;
  std::memcpy(wav.data() + 16, &fmt_size, 4);
  std::memcpy(wav.data() + 36, "JUNK", 4);
  const uint32_t junk_size = 4;
  std::memcpy(wav.data() + 40, &junk_size, 4);
  std::memcpy(wav.data() + 48, "data", 4);
  const uint32_t data_size = 100;
  std::memcpy(wav.data() + 52, &data_size, 4);

  TEST_ASSERT(DetectWavDataOffset(wav) == 56);
}

void TestDetectWavDataOffsetOddChunkAlignment() {
  using vinput::audio::DetectWavDataOffset;

  std::vector<uint8_t> wav(56 + 100, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "fmt ", 4);
  const uint32_t fmt_size = 16;
  std::memcpy(wav.data() + 16, &fmt_size, 4);
  std::memcpy(wav.data() + 36, "JUNK", 4);
  const uint32_t junk_size = 3;
  std::memcpy(wav.data() + 40, &junk_size, 4);
  std::memcpy(wav.data() + 48, "data", 4);
  const uint32_t data_size = 100;
  std::memcpy(wav.data() + 52, &data_size, 4);

  TEST_ASSERT(DetectWavDataOffset(wav) == 56);
}

void TestDetectWavDataOffsetFallback() {
  using vinput::audio::DetectWavDataOffset;

  std::vector<uint8_t> wav(44, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  TEST_ASSERT(DetectWavDataOffset(wav) == 44);
}

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
  TestDetectWavDataOffsetSmallBuffer();
  TestDetectWavDataOffsetRawPcm();
  TestDetectWavDataOffsetStandardWav();
  TestDetectWavDataOffsetWithPrecedingChunk();
  TestDetectWavDataOffsetOddChunkAlignment();
  TestDetectWavDataOffsetFallback();
  TestStreamWireProtocol();
  std::cout << "All stream pipeline tests passed successfully.\n";
  return 0;
}
