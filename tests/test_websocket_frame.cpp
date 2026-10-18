#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "common/net/websocket_frame.h"

namespace {

constexpr std::size_t kMaxTestPayload = 1024ULL * 1024;

} // namespace

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestHandshakeAcceptCalculation() {
  using vinput::net::ComputeWebSocketAccept;

  // RFC 6455 Section 1.3 standard test vector
  const auto accept1 = ComputeWebSocketAccept("dGhlIHNhbXBsZSBub25jZQ==");
  TEST_ASSERT(accept1.has_value());
  TEST_ASSERT(accept1.value_or("") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

  // Leading and trailing whitespace should be trimmed
  const auto accept2 = ComputeWebSocketAccept("  dGhlIHNhbXBsZSBub25jZQ== \r\n");
  TEST_ASSERT(accept2.has_value());
  TEST_ASSERT(accept2.value_or("") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

  // Empty or whitespace-only keys must be rejected
  TEST_ASSERT(!ComputeWebSocketAccept("").has_value());
  TEST_ASSERT(!ComputeWebSocketAccept("   \t\r\n").has_value());
}

void TestMaskingUtility() {
  using vinput::net::ApplyWebSocketMask;

  const std::uint8_t key[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  std::string data = "The quick brown fox jumps over the lazy dog";
  const std::string original = data;

  // Applying mask twice restores the original text (XOR symmetry)
  ApplyWebSocketMask(data.data(), data.size(), key, 0);
  TEST_ASSERT(data != original);

  ApplyWebSocketMask(data.data(), data.size(), key, 0);
  TEST_ASSERT(data == original);

  // Applying with offset (split in two chunks)
  std::string chunked = original;
  const std::size_t split_point = 11;
  ApplyWebSocketMask(chunked.data(), split_point, key, 0);
  ApplyWebSocketMask(chunked.data() + split_point, chunked.size() - split_point, key,
                     split_point % 4);

  std::string single_pass = original;
  ApplyWebSocketMask(single_pass.data(), single_pass.size(), key, 0);
  TEST_ASSERT(chunked == single_pass);

  // Edge cases: null or zero size should not crash
  ApplyWebSocketMask(nullptr, 0, key, 0);
  ApplyWebSocketMask(data.data(), 0, key, 0);
  ApplyWebSocketMask(data.data(), data.size(), nullptr, 0);
}

void TestParseHeaderIncompleteAndInvalid() {
  using vinput::net::ParseWebSocketFrameHeader;

  // Truncated buffers
  TEST_ASSERT(!ParseWebSocketFrameHeader("").has_value());
  TEST_ASSERT(!ParseWebSocketFrameHeader("\x81").has_value());

  // 16-bit extended length header truncated (< 4 bytes)
  const std::string trunc_16bit = std::string("\x81\x7e\x01", 3);
  TEST_ASSERT(!ParseWebSocketFrameHeader(trunc_16bit).has_value());

  // 64-bit extended length header truncated (< 10 bytes)
  const std::string trunc_64bit = std::string("\x81\x7f\x00\x00\x00", 5);
  TEST_ASSERT(!ParseWebSocketFrameHeader(trunc_64bit).has_value());

  // Masked frame header missing mask key
  const std::string trunc_mask = std::string("\x81\x85\x12\x34", 4);
  TEST_ASSERT(!ParseWebSocketFrameHeader(trunc_mask).has_value());

  // Reserved opcode (e.g. 0x3, 0x7, 0xB, 0xF)
  TEST_ASSERT(!ParseWebSocketFrameHeader(std::string_view("\x83\x00", 2)).has_value());
  TEST_ASSERT(!ParseWebSocketFrameHeader(std::string_view("\x8F\x00", 2)).has_value());

  // RSV1-3 bits set (RFC 6455 5.2: must be 0)
  TEST_ASSERT(!ParseWebSocketFrameHeader(std::string_view("\xC1\x00", 2)).has_value()); // RSV1
  TEST_ASSERT(!ParseWebSocketFrameHeader(std::string_view("\xA1\x00", 2)).has_value()); // RSV2
  TEST_ASSERT(!ParseWebSocketFrameHeader(std::string_view("\x91\x00", 2)).has_value()); // RSV3

  // Non-canonical length encodings (RFC 6455 5.2)
  // 16-bit encoding with length < 126
  TEST_ASSERT(!ParseWebSocketFrameHeader(std::string_view("\x81\x7e\x00\x05", 4)).has_value());
  // 64-bit encoding with length <= 65535
  TEST_ASSERT(
      !ParseWebSocketFrameHeader(std::string_view("\x81\x7f\x00\x00\x00\x00\x00\x00\x01\x00", 10))
           .has_value());

  // Close frame with exactly 1 byte payload (RFC 6455 5.5.1: invalid)
  TEST_ASSERT(!ParseWebSocketFrameHeader(std::string_view("\x88\x01", 2)).has_value());

  // Control frame fragmented (RFC 6455 5.5: must NOT be fragmented)
  TEST_ASSERT(
      !ParseWebSocketFrameHeader(std::string_view("\x09\x00", 2)).has_value()); // Ping FIN=0

  // Control frame with payload length > 125
  const std::string ping_too_large = std::string("\x89\x7e\x00\x80", 4); // 128 bytes
  TEST_ASSERT(!ParseWebSocketFrameHeader(ping_too_large).has_value());

  // 64-bit length with MSB set (RFC 6455 5.2: MSB must be 0)
  const std::string msb_set = std::string("\x81\x7f\x80\x00\x00\x00\x00\x00\x00\x01", 10);
  TEST_ASSERT(!ParseWebSocketFrameHeader(msb_set).has_value());

  // Positive control assertion: valid 0-byte Ping frame parses successfully
  TEST_ASSERT(ParseWebSocketFrameHeader(std::string_view("\x89\x00", 2)).has_value());
}

void TestShortFrameCodec() {
  using vinput::net::DecodeWebSocketFrame;
  using vinput::net::EncodeWebSocketFrame;
  using vinput::net::WsDecodeStatus;
  using vinput::net::WsFrame;
  using vinput::net::WsOpcode;

  const std::string text = "Hello, fcitx5-vinput!";

  // 1. Unmasked Text frame
  const std::string wire_unmasked =
      EncodeWebSocketFrame(WsOpcode::kText, text, /*fin=*/true, nullptr);
  TEST_ASSERT(wire_unmasked.size() == 2 + text.size());

  WsFrame frame1;
  std::size_t consumed1 = 0;
  std::string error1;
  TEST_ASSERT(DecodeWebSocketFrame(wire_unmasked, &frame1, &consumed1, kMaxTestPayload, &error1) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(consumed1 == wire_unmasked.size());
  TEST_ASSERT(frame1.header.fin);
  TEST_ASSERT(frame1.header.opcode == WsOpcode::kText);
  TEST_ASSERT(!frame1.header.masked);
  TEST_ASSERT(frame1.header.payload_length == text.size());
  TEST_ASSERT(frame1.payload == text);

  // 2. Masked Text frame (Client -> Server)
  const std::uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  const std::string wire_masked = EncodeWebSocketFrame(WsOpcode::kText, text, /*fin=*/true, mask);
  TEST_ASSERT(wire_masked.size() == 2 + 4 + text.size());
  // Verify that the payload on the wire is actually masked
  TEST_ASSERT(wire_masked.substr(6) != text);

  WsFrame frame2;
  std::size_t consumed2 = 0;
  std::string error2;
  TEST_ASSERT(DecodeWebSocketFrame(wire_masked, &frame2, &consumed2, kMaxTestPayload, &error2) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(consumed2 == wire_masked.size());
  TEST_ASSERT(frame2.header.fin);
  TEST_ASSERT(frame2.header.opcode == WsOpcode::kText);
  TEST_ASSERT(frame2.header.masked);
  TEST_ASSERT(frame2.header.mask_key[0] == 0x12);
  TEST_ASSERT(frame2.header.mask_key[1] == 0x34);
  TEST_ASSERT(frame2.header.mask_key[2] == 0x56);
  TEST_ASSERT(frame2.header.mask_key[3] == 0x78);
  TEST_ASSERT(frame2.header.payload_length == text.size());
  TEST_ASSERT(frame2.payload == text);
}

void TestMediumAndLargeFrameCodec() {
  using vinput::net::DecodeWebSocketFrame;
  using vinput::net::EncodeWebSocketFrame;
  using vinput::net::WsDecodeStatus;
  using vinput::net::WsFrame;
  using vinput::net::WsOpcode;

  // Medium frame: 126 <= len <= 65535 (uses 2-byte extended length)
  std::string medium_data;
  medium_data.resize(500);
  for (std::size_t i = 0; i < medium_data.size(); ++i) {
    medium_data[i] = static_cast<char>((i * 37) & 0xFF);
  }

  const std::uint8_t mask[4] = {0xFE, 0xDC, 0xBA, 0x98};
  const std::string wire_medium =
      EncodeWebSocketFrame(WsOpcode::kBinary, medium_data, /*fin=*/true, mask);
  TEST_ASSERT(wire_medium.size() == 2 + 2 + 4 + medium_data.size());

  WsFrame frame_med;
  std::size_t consumed_med = 0;
  TEST_ASSERT(DecodeWebSocketFrame(wire_medium, &frame_med, &consumed_med, kMaxTestPayload) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(consumed_med == wire_medium.size());
  TEST_ASSERT(frame_med.header.fin);
  TEST_ASSERT(frame_med.header.opcode == WsOpcode::kBinary);
  TEST_ASSERT(frame_med.header.masked);
  TEST_ASSERT(frame_med.header.payload_length == 500);
  TEST_ASSERT(frame_med.payload == medium_data);

  // Large frame: len > 65535 (uses 8-byte extended length)
  std::string large_data;
  large_data.resize(70000, 'Z');
  const std::string wire_large =
      EncodeWebSocketFrame(WsOpcode::kText, large_data, /*fin=*/true, nullptr);
  TEST_ASSERT(wire_large.size() == 2 + 8 + large_data.size());

  WsFrame frame_large;
  std::size_t consumed_large = 0;
  TEST_ASSERT(DecodeWebSocketFrame(wire_large, &frame_large, &consumed_large, kMaxTestPayload) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(consumed_large == wire_large.size());
  TEST_ASSERT(frame_large.header.fin);
  TEST_ASSERT(frame_large.header.opcode == WsOpcode::kText);
  TEST_ASSERT(!frame_large.header.masked);
  TEST_ASSERT(frame_large.header.payload_length == 70000);
  TEST_ASSERT(frame_large.payload == large_data);
}

void TestStreamingAndIncompleteBuffer() {
  using vinput::net::DecodeWebSocketFrame;
  using vinput::net::EncodeWebSocketText;
  using vinput::net::WsDecodeStatus;
  using vinput::net::WsFrame;

  const std::string wire = EncodeWebSocketText("Streaming payload test");

  WsFrame frame;
  std::size_t consumed = 0;
  std::string error;

  // Feed byte-by-byte up to 1 byte before complete frame
  for (std::size_t i = 1; i < wire.size(); ++i) {
    const std::string_view partial(wire.data(), i);
    consumed = 999;
    const auto status = DecodeWebSocketFrame(partial, &frame, &consumed, kMaxTestPayload, &error);
    TEST_ASSERT(status == WsDecodeStatus::kNeedMoreData);
    TEST_ASSERT(consumed == 0);
  }

  // Full frame feed
  TEST_ASSERT(DecodeWebSocketFrame(wire, &frame, &consumed, kMaxTestPayload, &error) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(consumed == wire.size());
  TEST_ASSERT(frame.payload == "Streaming payload test");

  // Stream with trailing bytes for next frame
  std::string multi_stream = wire;
  const std::string wire2 = EncodeWebSocketText("Second message");
  multi_stream.append(wire2);

  WsFrame frame1;
  std::size_t consumed1 = 0;
  TEST_ASSERT(DecodeWebSocketFrame(multi_stream, &frame1, &consumed1) == WsDecodeStatus::kOk);
  TEST_ASSERT(consumed1 == wire.size());
  TEST_ASSERT(frame1.payload == "Streaming payload test");

  const std::string_view remaining(multi_stream.data() + consumed1,
                                   multi_stream.size() - consumed1);
  WsFrame frame2;
  std::size_t consumed2 = 0;
  TEST_ASSERT(DecodeWebSocketFrame(remaining, &frame2, &consumed2) == WsDecodeStatus::kOk);
  TEST_ASSERT(consumed2 == wire2.size());
  TEST_ASSERT(frame2.payload == "Second message");
}

void TestPayloadLimitExceeded() {
  using vinput::net::DecodeWebSocketFrame;
  using vinput::net::EncodeWebSocketText;
  using vinput::net::WsDecodeStatus;
  using vinput::net::WsFrame;

  const std::string wire = EncodeWebSocketText(std::string(1024, 'X'));
  WsFrame frame;
  std::size_t consumed = 0;
  std::string error;

  // Max payload is 512 bytes, frame is 1024 bytes
  const auto status = DecodeWebSocketFrame(wire, &frame, &consumed, 512, &error);
  TEST_ASSERT(status == WsDecodeStatus::kPayloadTooLarge);
  TEST_ASSERT(!error.empty());
}

void TestProtocolErrorDecode() {
  using vinput::net::DecodeWebSocketFrame;
  using vinput::net::WsDecodeStatus;
  using vinput::net::WsFrame;

  WsFrame frame;
  std::size_t consumed = 0;
  std::string error;

  // RSV bit set
  TEST_ASSERT(DecodeWebSocketFrame(std::string_view("\xC1\x00", 2), &frame, &consumed,
                                   kMaxTestPayload, &error) == WsDecodeStatus::kProtocolError);

  // Invalid opcode
  TEST_ASSERT(DecodeWebSocketFrame(std::string_view("\x83\x00", 2), &frame, &consumed,
                                   kMaxTestPayload, &error) == WsDecodeStatus::kProtocolError);

  // Non-canonical 16-bit length
  TEST_ASSERT(DecodeWebSocketFrame(std::string_view("\x81\x7e\x00\x05Hello", 9), &frame, &consumed,
                                   kMaxTestPayload, &error) == WsDecodeStatus::kProtocolError);

  // Close frame with 1 byte payload
  TEST_ASSERT(DecodeWebSocketFrame(std::string_view("\x88\x01\x00", 3), &frame, &consumed,
                                   kMaxTestPayload, &error) == WsDecodeStatus::kProtocolError);

  // Close frame with prohibited status code 1005 (0x03ED)
  TEST_ASSERT(DecodeWebSocketFrame(std::string_view("\x88\x02\x03\xED", 4), &frame, &consumed,
                                   kMaxTestPayload, &error) == WsDecodeStatus::kProtocolError);

  // Close frame with status code 0 (0x0000)
  TEST_ASSERT(DecodeWebSocketFrame(std::string_view("\x88\x02\x00\x00", 4), &frame, &consumed,
                                   kMaxTestPayload, &error) == WsDecodeStatus::kProtocolError);

  // Close frame with valid code 1000 but invalid UTF-8 reason (\xFF\xFF)
  TEST_ASSERT(DecodeWebSocketFrame(std::string_view("\x88\x04\x03\xE8\xFF\xFF", 6), &frame,
                                   &consumed, kMaxTestPayload,
                                   &error) == WsDecodeStatus::kProtocolError);
}

void TestFragmentedFrameAndControlHelpers() {
  using vinput::net::DecodeWebSocketFrame;
  using vinput::net::EncodeWebSocketBinary;
  using vinput::net::EncodeWebSocketClose;
  using vinput::net::EncodeWebSocketFrame;
  using vinput::net::EncodeWebSocketPing;
  using vinput::net::EncodeWebSocketPong;
  using vinput::net::WsDecodeStatus;
  using vinput::net::WsFrame;
  using vinput::net::WsOpcode;

  // 1. Fragmented frames: Text (FIN=0) followed by Continuation (FIN=1)
  const std::string frag1 = EncodeWebSocketFrame(WsOpcode::kText, "Part 1 ", /*fin=*/false);
  const std::string frag2 = EncodeWebSocketFrame(WsOpcode::kContinuation, "Part 2", /*fin=*/true);

  WsFrame f1;
  std::size_t c1 = 0;
  TEST_ASSERT(DecodeWebSocketFrame(frag1, &f1, &c1) == WsDecodeStatus::kOk);
  TEST_ASSERT(!f1.header.fin);
  TEST_ASSERT(f1.header.opcode == WsOpcode::kText);
  TEST_ASSERT(f1.payload == "Part 1 ");

  WsFrame f2;
  std::size_t c2 = 0;
  TEST_ASSERT(DecodeWebSocketFrame(frag2, &f2, &c2) == WsDecodeStatus::kOk);
  TEST_ASSERT(f2.header.fin);
  TEST_ASSERT(f2.header.opcode == WsOpcode::kContinuation);
  TEST_ASSERT(f2.payload == "Part 2");

  // 2. Ping / Pong helpers (including oversized payload truncation to 125 bytes)
  const std::string ping_wire = EncodeWebSocketPing("heartbeat");
  WsFrame ping_frame;
  std::size_t c_ping = 0;
  TEST_ASSERT(DecodeWebSocketFrame(ping_wire, &ping_frame, &c_ping) == WsDecodeStatus::kOk);
  TEST_ASSERT(ping_frame.header.fin);
  TEST_ASSERT(ping_frame.header.opcode == WsOpcode::kPing);
  TEST_ASSERT(ping_frame.payload == "heartbeat");

  const std::string ping_oversized = EncodeWebSocketPing(std::string(200, 'P'));
  WsFrame ping_oversized_frame;
  std::size_t c_ping_over = 0;
  TEST_ASSERT(DecodeWebSocketFrame(ping_oversized, &ping_oversized_frame, &c_ping_over) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(ping_oversized_frame.header.payload_length == 125);
  TEST_ASSERT(ping_oversized_frame.payload.size() == 125);

  const std::string pong_wire = EncodeWebSocketPong("heartbeat");
  WsFrame pong_frame;
  std::size_t c_pong = 0;
  TEST_ASSERT(DecodeWebSocketFrame(pong_wire, &pong_frame, &c_pong) == WsDecodeStatus::kOk);
  TEST_ASSERT(pong_frame.header.fin);
  TEST_ASSERT(pong_frame.header.opcode == WsOpcode::kPong);
  TEST_ASSERT(pong_frame.payload == "heartbeat");

  // 3. Binary helper
  const std::string bin_wire = EncodeWebSocketBinary("\x01\x02\x03\x04");
  WsFrame bin_frame;
  std::size_t c_bin = 0;
  TEST_ASSERT(DecodeWebSocketFrame(bin_wire, &bin_frame, &c_bin) == WsDecodeStatus::kOk);
  TEST_ASSERT(bin_frame.header.fin);
  TEST_ASSERT(bin_frame.header.opcode == WsOpcode::kBinary);
  TEST_ASSERT(bin_frame.payload == "\x01\x02\x03\x04");

  // 4. Close frame helper
  // Standard normal closure (code 1000, reason "Session ended")
  const std::string close_wire1 = EncodeWebSocketClose(1000, "Session ended");
  WsFrame close_frame1;
  std::size_t c_close1 = 0;
  TEST_ASSERT(DecodeWebSocketFrame(close_wire1, &close_frame1, &c_close1) == WsDecodeStatus::kOk);
  TEST_ASSERT(close_frame1.header.fin);
  TEST_ASSERT(close_frame1.header.opcode == WsOpcode::kClose);
  TEST_ASSERT(close_frame1.payload.size() == 2 + std::string("Session ended").size());
  const auto status1 =
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(close_frame1.payload[0])) << 8) |
      static_cast<std::uint8_t>(close_frame1.payload[1]);
  TEST_ASSERT(status1 == 1000);
  TEST_ASSERT(close_frame1.payload.substr(2) == "Session ended");

  // Empty close frame (code 0, empty reason)
  const std::string close_wire2 = EncodeWebSocketClose(0, "");
  WsFrame close_frame2;
  std::size_t c_close2 = 0;
  TEST_ASSERT(DecodeWebSocketFrame(close_wire2, &close_frame2, &c_close2) == WsDecodeStatus::kOk);
  TEST_ASSERT(close_frame2.header.fin);
  TEST_ASSERT(close_frame2.header.opcode == WsOpcode::kClose);
  TEST_ASSERT(close_frame2.payload.empty());

  // Code 0 with non-empty reason: reason should be ignored, status code 0 must NOT be sent
  const std::string close_wire_zero = EncodeWebSocketClose(0, "ignored reason");
  WsFrame close_frame_zero;
  std::size_t c_close_zero = 0;
  TEST_ASSERT(DecodeWebSocketFrame(close_wire_zero, &close_frame_zero, &c_close_zero) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(close_frame_zero.payload.empty());

  // Prohibited status codes (1005, 1006) must not be sent on the wire
  const std::string close_wire_1005 = EncodeWebSocketClose(1005, "should be empty");
  WsFrame close_frame_1005;
  std::size_t c_close_1005 = 0;
  TEST_ASSERT(DecodeWebSocketFrame(close_wire_1005, &close_frame_1005, &c_close_1005) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(close_frame_1005.payload.empty());

  // Long UTF-8 reason string: must be truncated to <= 123 bytes without splitting UTF-8 characters
  // "测试" is 6 bytes (3 bytes each). Repeat 30 times = 180 bytes.
  std::string long_utf8;
  for (int i = 0; i < 30; ++i) {
    long_utf8 += "测试";
  }
  const std::string close_wire_utf8 = EncodeWebSocketClose(1000, long_utf8);
  WsFrame close_frame_utf8;
  std::size_t c_close_utf8 = 0;
  TEST_ASSERT(DecodeWebSocketFrame(close_wire_utf8, &close_frame_utf8, &c_close_utf8) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(close_frame_utf8.payload.size() <= 125);
  // Invalid UTF-8 sequence in reason: must stop before invalid byte
  const std::string close_wire_bad_utf8 = EncodeWebSocketClose(1000, "Valid\xFF\xFEInvalid");
  WsFrame close_frame_bad_utf8;
  std::size_t c_close_bad = 0;
  TEST_ASSERT(DecodeWebSocketFrame(close_wire_bad_utf8, &close_frame_bad_utf8, &c_close_bad) ==
              WsDecodeStatus::kOk);
  TEST_ASSERT(close_frame_bad_utf8.payload.substr(2) == "Valid");
}

int main() {
  std::printf("Running WebSocket frame test suite...\n");

  TestHandshakeAcceptCalculation();
  TestMaskingUtility();
  TestParseHeaderIncompleteAndInvalid();
  TestShortFrameCodec();
  TestMediumAndLargeFrameCodec();
  TestStreamingAndIncompleteBuffer();
  TestPayloadLimitExceeded();
  TestProtocolErrorDecode();
  TestFragmentedFrameAndControlHelpers();

  std::printf("All WebSocket frame tests passed successfully!\n");
  return 0;
}
