#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
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

// Walks RIFF chunks of `buffer` and returns the `data` bounds, or nullopt when the
// buffer does not yet contain a complete `data` chunk header (or is malformed).
std::optional<vinput::audio::WavDataBounds> FindDataBounds(std::span<const uint8_t> buffer) {
  std::size_t scan_pos = vinput::audio::kRiffHeaderSize;
  const auto outcome = vinput::audio::WalkWavChunks(buffer, scan_pos);
  if (outcome.result == vinput::audio::WavScanResult::DataFound) {
    return outcome.bounds;
  }
  return std::nullopt; // NeedMoreData or Malformed
}

void TestWavScanSmallBuffer() {
  using vinput::audio::StartsWithWavPrefix;

  std::vector<uint8_t> empty;
  TEST_ASSERT(!StartsWithWavPrefix(empty));
  TEST_ASSERT(!FindDataBounds(empty).has_value());

  std::vector<uint8_t> small(20, 0);
  TEST_ASSERT(!StartsWithWavPrefix(small));
  TEST_ASSERT(!FindDataBounds(small).has_value());
}

void TestWavScanRawPcm() {
  using vinput::audio::StartsWithWavPrefix;

  std::vector<uint8_t> pcm(100, 0x55);
  TEST_ASSERT(!StartsWithWavPrefix(pcm));
  TEST_ASSERT(!FindDataBounds(pcm).has_value());
}

void TestWavScanStandardWav() {
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

  const auto bounds = FindDataBounds(wav);
  TEST_ASSERT(bounds.has_value() && bounds->offset == 44);
  TEST_ASSERT(bounds.has_value() && bounds->size == 100);
}

void TestWavScanWithPrecedingChunk() {
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

  const auto bounds = FindDataBounds(wav);
  TEST_ASSERT(bounds.has_value() && bounds->offset == 56);
  TEST_ASSERT(bounds.has_value() && bounds->size == 100);
}

void TestWavScanOddChunkAlignment() {
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

  // Odd-sized JUNK payloads are padded to the next even boundary.
  const auto bounds = FindDataBounds(wav);
  TEST_ASSERT(bounds.has_value() && bounds->offset == 56);
}

void TestWavScanStreamingSizeNonDataChunk() {
  // A non-data chunk declaring the streaming sentinel size (0xFFFFFFFF) must not wrap
  // its padded extent to zero: the bytes that follow, even when they spell "data", are
  // payload of that chunk and must not be reported as audio bounds.
  std::vector<uint8_t> wav(12 + 8 + 8, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "JUNK", 4);
  const uint32_t streaming_size = 0xFFFFFFFFU;
  std::memcpy(wav.data() + 16, &streaming_size, 4);
  std::memcpy(wav.data() + 20, "data", 4);
  const uint32_t data_size = 4;
  std::memcpy(wav.data() + 24, &data_size, 4);

  const auto bounds = FindDataBounds(wav);
  TEST_ASSERT(!bounds.has_value());
}

void TestWavScanMissingDataChunk() {
  // RIFF/WAVE with only a fmt chunk: the scan must stay pending (need more data)
  // instead of fabricating an offset-44 fallback.
  std::vector<uint8_t> wav(144, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "fmt ", 4);
  const uint32_t fmt_size = 128;
  std::memcpy(wav.data() + 16, &fmt_size, 4);

  const auto bounds = FindDataBounds(wav);
  TEST_ASSERT(!bounds.has_value());
}

void TestWavScanMalformedChunkSize() {
  std::vector<uint8_t> wav(56 + 100, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "JUNK", 4);
  const uint32_t huge_size = 0xFFFFFFF0U;
  std::memcpy(wav.data() + 16, &huge_size, 4);

  TEST_ASSERT(!FindDataBounds(wav).has_value());
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
  TestWavScanSmallBuffer();
  TestWavScanRawPcm();
  TestWavScanStandardWav();
  TestWavScanWithPrecedingChunk();
  TestWavScanOddChunkAlignment();
  TestWavScanStreamingSizeNonDataChunk();
  TestWavScanMissingDataChunk();
  TestWavScanMalformedChunkSize();
  TestStreamWireProtocol();
  std::cout << "All stream pipeline tests passed successfully.\n";
  return 0;
}
