#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vinput::net {

enum class WsOpcode : std::uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct WsFrameHeader {
  bool fin = true;
  WsOpcode opcode = WsOpcode::kText;
  bool masked = false;
  std::uint64_t payload_length = 0;
  std::uint8_t mask_key[4] = {};
  std::size_t header_size = 0;
};

struct WsFrame {
  WsFrameHeader header;
  std::string payload;
};

// Parses the WebSocket frame header from buffer without consuming payload.
// Returns std::nullopt if the buffer does not yet contain a complete and valid header.
std::optional<WsFrameHeader> ParseWebSocketFrameHeader(std::string_view buffer);

// Decodes a complete WebSocket frame from buffer.
// If successful, stores frame into *out_frame, bytes consumed into *consumed_bytes, and returns
// true. If buffer does not yet contain the full frame payload, sets *consumed_bytes = 0 and returns
// false. If frame violates protocol rules or exceeds max_payload_bytes, sets error message and
// returns false.
bool DecodeWebSocketFrame(std::string_view buffer, WsFrame* out_frame, std::size_t* consumed_bytes,
                          std::size_t max_payload_bytes = 16ULL * 1024 * 1024,
                          std::string* error = nullptr);

// Encodes a WebSocket frame.
// If mask_key is non-null, masks the payload with mask_key.
std::string EncodeWebSocketFrame(WsOpcode opcode, std::string_view payload, bool fin = true,
                                 const std::uint8_t* mask_key = nullptr);

// Applies RFC 6455 4-byte XOR mask in-place to data with optional byte offset.
void ApplyWebSocketMask(char* data, std::size_t length, const std::uint8_t mask_key[4],
                        std::size_t offset = 0);

// Computes RFC 6455 Sec-WebSocket-Accept response header value from client key.
// Returns std::nullopt if client_key is empty or SHA-1 computation fails.
std::optional<std::string> ComputeWebSocketAccept(std::string_view client_key);

// Convenience encoding helpers
std::string EncodeWebSocketText(std::string_view text, bool fin = true);
std::string EncodeWebSocketBinary(std::string_view binary, bool fin = true);
std::string EncodeWebSocketPing(std::string_view payload = {});
std::string EncodeWebSocketPong(std::string_view payload = {});
std::string EncodeWebSocketClose(std::uint16_t status_code = 1000, std::string_view reason = {});

} // namespace vinput::net
