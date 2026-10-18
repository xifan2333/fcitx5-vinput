#include "common/net/websocket_frame.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/types.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "common/utils/string_utils.h"

namespace vinput::net {

namespace {

inline void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    *error = message;
  }
}

// Bitmask check: valid opcodes are 0x0, 0x1, 0x2, 0x8, 0x9, 0xA.
// Bit indices: 0, 1, 2, 8, 9, 10 -> (1<<0)|(1<<1)|(1<<2)|(1<<8)|(1<<9)|(1<<10) = 0x0707
constexpr bool IsValidOpcode(std::uint8_t opcode) noexcept {
  return opcode <= 0x0A && ((1U << opcode) & 0x0707U) != 0;
}

constexpr bool IsControlOpcode(WsOpcode opcode) noexcept {
  return static_cast<std::uint8_t>(opcode) >= 0x08;
}

// RFC 6455 7.4: valid close status codes to send over the wire
// 1000..1014 (excluding 1004, 1005, 1006) and 3000..4999
constexpr bool IsValidCloseStatusCode(std::uint16_t code) noexcept {
  if (code >= 1000 && code <= 1014) {
    return ((1U << (code - 1000)) & 0x7F8FU) != 0;
  }
  return code >= 3000 && code <= 4999;
}

struct LenDescriptor {
  std::uint8_t ext_bytes;
  std::uint64_t min_canonical_len;
};

constexpr LenDescriptor kLenDescriptors[3] = {
    {0, 0},     // 0..125
    {2, 126},   // 126: requires 2 extended bytes, payload >= 126
    {8, 65536}, // 127: requires 8 extended bytes, payload >= 65536
};

constexpr const LenDescriptor& GetLenDescriptor(std::uint8_t len_code) noexcept {
  if (len_code < 126) {
    return kLenDescriptors[0];
  }
  if (len_code == 126) {
    return kLenDescriptors[1];
  }
  return kLenDescriptors[2];
}

} // namespace

WsDecodeStatus ParseWebSocketFrameHeader(std::string_view buffer, WsFrameHeader* out_header,
                                         std::string* error) {
  if (out_header == nullptr) {
    SetError(error, "Invalid output header pointer");
    return WsDecodeStatus::kProtocolError;
  }

  // Guard 1: Basic 2-byte header availability
  if (buffer.size() < 2) {
    SetError(error, "Incomplete frame header: buffer too short");
    return WsDecodeStatus::kNeedMoreData;
  }

  const auto byte0 = static_cast<std::uint8_t>(buffer[0]);
  const auto byte1 = static_cast<std::uint8_t>(buffer[1]);

  // Guard 2: RSV bits must be 0 (RFC 6455 5.2)
  if ((byte0 & 0x70) != 0) {
    SetError(error, "WebSocket reserved RSV bits must be 0");
    return WsDecodeStatus::kProtocolError;
  }

  // Guard 3: Valid opcode check via bitmask table
  const auto opcode_val = static_cast<std::uint8_t>(byte0 & 0x0F);
  if (!IsValidOpcode(opcode_val)) {
    SetError(error, "Invalid WebSocket opcode");
    return WsDecodeStatus::kProtocolError;
  }

  const std::uint8_t len_code = byte1 & 0x7F;
  const bool masked = (byte1 & 0x80) != 0;
  const auto& desc = GetLenDescriptor(len_code);

  // Guard 4: Full header availability (2 base + ext_bytes + mask_bytes)
  const std::size_t header_size = 2 + desc.ext_bytes + (masked ? 4 : 0);
  if (buffer.size() < header_size) {
    SetError(error, "Incomplete frame header: waiting for extended length or mask");
    return WsDecodeStatus::kNeedMoreData;
  }

  // Extract payload length based on descriptor
  std::uint64_t payload_len = len_code;
  if (desc.ext_bytes == 2) {
    payload_len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[2])) << 8) |
                  static_cast<std::uint8_t>(buffer[3]);
  } else if (desc.ext_bytes == 8) {
    payload_len = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      payload_len = (payload_len << 8) | static_cast<std::uint8_t>(buffer[2 + i]);
    }
    // Guard 5: MSB must be 0 (RFC 6455 5.2)
    if ((payload_len & (1ULL << 63)) != 0) {
      SetError(error, "WebSocket 64-bit length has most significant bit set");
      return WsDecodeStatus::kProtocolError;
    }
  }

  // Guard 6: Minimal canonical length enforcement (RFC 6455 5.2)
  if (payload_len < desc.min_canonical_len) {
    SetError(error, "Non-minimal WebSocket payload length encoding");
    return WsDecodeStatus::kProtocolError;
  }

  const bool fin = (byte0 & 0x80) != 0;
  const auto opcode = static_cast<WsOpcode>(opcode_val);

  // Guard 7: Control frame constraints (RFC 6455 5.5, 5.5.1)
  if (IsControlOpcode(opcode)) {
    if (!fin) {
      SetError(error, "Control frames MUST NOT be fragmented");
      return WsDecodeStatus::kProtocolError;
    }
    if (payload_len > 125) {
      SetError(error, "Control frame payload exceeds 125 bytes");
      return WsDecodeStatus::kProtocolError;
    }
    if (opcode == WsOpcode::kClose && payload_len == 1) {
      SetError(error, "Close frame payload cannot be exactly 1 byte");
      return WsDecodeStatus::kProtocolError;
    }
  }

  // Extract mask key if present
  if (masked) {
    const std::size_t mask_offset = 2 + desc.ext_bytes;
    std::memcpy(out_header->mask_key, buffer.data() + mask_offset, 4);
  } else {
    std::memset(out_header->mask_key, 0, 4);
  }

  out_header->fin = fin;
  out_header->opcode = opcode;
  out_header->masked = masked;
  out_header->payload_length = payload_len;
  out_header->header_size = header_size;
  return WsDecodeStatus::kOk;
}

std::optional<WsFrameHeader> ParseWebSocketFrameHeader(std::string_view buffer) {
  WsFrameHeader header;
  if (ParseWebSocketFrameHeader(buffer, &header) == WsDecodeStatus::kOk) {
    return header;
  }
  return std::nullopt;
}

WsDecodeStatus DecodeWebSocketFrame(std::string_view buffer, WsFrame* out_frame,
                                    std::size_t* consumed_bytes, std::size_t max_payload_bytes,
                                    std::string* error) {
  if (consumed_bytes != nullptr) {
    *consumed_bytes = 0;
  }
  if (out_frame == nullptr || consumed_bytes == nullptr) {
    SetError(error, "Invalid output pointer in DecodeWebSocketFrame");
    return WsDecodeStatus::kProtocolError;
  }

  WsFrameHeader header;
  const WsDecodeStatus header_status = ParseWebSocketFrameHeader(buffer, &header, error);
  if (header_status != WsDecodeStatus::kOk) {
    return header_status;
  }

  // Guard: Payload size limit
  if (header.payload_length > max_payload_bytes) {
    SetError(error, "WebSocket frame payload exceeds maximum allowed size");
    return WsDecodeStatus::kPayloadTooLarge;
  }

  // Guard: Complete payload availability in buffer
  if (buffer.size() < header.header_size + header.payload_length) {
    SetError(error, "Incomplete frame payload");
    return WsDecodeStatus::kNeedMoreData;
  }

  std::string payload(buffer.substr(header.header_size, header.payload_length));
  if (header.masked) {
    ApplyWebSocketMask(payload.data(), payload.size(), header.mask_key, 0);
  }

  // Guard: Inbound close frame status code validation (RFC 6455 7.4)
  if (header.opcode == WsOpcode::kClose && payload.size() >= 2) {
    const auto status_code =
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[0])) << 8) |
        static_cast<std::uint8_t>(payload[1]);
    if (!IsValidCloseStatusCode(status_code)) {
      SetError(error, "Invalid WebSocket Close status code");
      return WsDecodeStatus::kProtocolError;
    }
    const std::string_view reason(payload.data() + 2, payload.size() - 2);
    if (!vinput::str::IsValidUtf8(reason)) {
      SetError(error, "WebSocket Close reason is not valid UTF-8");
      return WsDecodeStatus::kProtocolError;
    }
  }

  out_frame->header = header;
  out_frame->payload = std::move(payload);
  *consumed_bytes = header.header_size + header.payload_length;
  return WsDecodeStatus::kOk;
}

std::string EncodeWebSocketFrame(WsOpcode opcode, std::string_view payload, bool fin,
                                 const std::uint8_t* mask_key) {
  // Enforce RFC 6455 5.5 control frame rules
  if (IsControlOpcode(opcode)) {
    fin = true;
    if (payload.size() > 125) {
      payload = payload.substr(0, 125);
    }
  }

  const std::size_t payload_len = payload.size();
  std::uint8_t len_code = 127;
  if (payload_len < 126) {
    len_code = static_cast<std::uint8_t>(payload_len);
  } else if (payload_len <= 0xFFFF) {
    len_code = 126;
  }
  const auto& desc = GetLenDescriptor(len_code);

  std::string frame;
  frame.reserve(2 + desc.ext_bytes + (mask_key != nullptr ? 4 : 0) + payload_len);

  // Byte 0: FIN + Opcode
  const auto byte0 =
      static_cast<char>((fin ? 0x80 : 0x00) | (static_cast<std::uint8_t>(opcode) & 0x0F));
  frame.push_back(byte0);

  // Byte 1: Mask flag + len_code
  const std::uint8_t mask_flag = (mask_key != nullptr) ? 0x80 : 0x00;
  frame.push_back(static_cast<char>(mask_flag | len_code));

  // Extended length bytes
  if (desc.ext_bytes == 2) {
    frame.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(payload_len & 0xFF));
  } else if (desc.ext_bytes == 8) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload_len) >> shift) & 0xFF));
    }
  }

  // Mask key and payload
  if (mask_key != nullptr) {
    const std::span<const std::uint8_t, 4> mask(mask_key, 4);
    for (const auto byte : mask) {
      frame.push_back(static_cast<char>(byte));
    }
    const std::size_t payload_start = frame.size();
    frame.append(payload);
    ApplyWebSocketMask(frame.data() + payload_start, payload.size(), mask_key, 0);
  } else {
    frame.append(payload);
  }

  return frame;
}

void ApplyWebSocketMask(char* data, std::size_t length, const std::uint8_t mask_key[4],
                        std::size_t offset) {
  if (data == nullptr || length == 0 || mask_key == nullptr) {
    return;
  }
  const std::span<const std::uint8_t, 4> mask(mask_key, 4);
  for (std::size_t i = 0; i < length; ++i) {
    data[i] = static_cast<char>(static_cast<std::uint8_t>(data[i]) ^ mask[(offset + i) % 4]);
  }
}

std::optional<std::string> ComputeWebSocketAccept(std::string_view client_key) {
  const std::string_view trimmed_key = vinput::str::TrimAsciiWhitespaceView(client_key);
  if (trimmed_key.empty()) {
    return std::nullopt;
  }
  std::string combined;
  combined.reserve(trimmed_key.size() + kWebSocketGuid.size());
  combined.append(trimmed_key);
  combined.append(kWebSocketGuid);

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    return std::nullopt;
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;
  const bool ok = EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1 &&
                  EVP_DigestUpdate(ctx, combined.data(), combined.size()) == 1 &&
                  EVP_DigestFinal_ex(ctx, digest, &digest_size) == 1;
  EVP_MD_CTX_free(ctx);
  if (!ok || digest_size == 0) {
    return std::nullopt;
  }
  return vinput::str::Base64Encode(digest, digest_size);
}

std::string EncodeWebSocketText(std::string_view text, bool fin) {
  return EncodeWebSocketFrame(WsOpcode::kText, text, fin, nullptr);
}

std::string EncodeWebSocketBinary(std::string_view binary, bool fin) {
  return EncodeWebSocketFrame(WsOpcode::kBinary, binary, fin, nullptr);
}

std::string EncodeWebSocketPing(std::string_view payload) {
  if (payload.size() > 125) {
    payload = payload.substr(0, 125);
  }
  return EncodeWebSocketFrame(WsOpcode::kPing, payload, true, nullptr);
}

std::string EncodeWebSocketPong(std::string_view payload) {
  if (payload.size() > 125) {
    payload = payload.substr(0, 125);
  }
  return EncodeWebSocketFrame(WsOpcode::kPong, payload, true, nullptr);
}

std::string EncodeWebSocketClose(std::uint16_t status_code, std::string_view reason) {
  std::string payload;
  if (IsValidCloseStatusCode(status_code)) {
    const std::string_view safe_reason = vinput::str::TruncateUtf8Bytes(reason, 125 - 2);
    payload.reserve(2 + safe_reason.size());
    payload.push_back(static_cast<char>((status_code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(status_code & 0xFF));
    payload.append(safe_reason);
  }
  return EncodeWebSocketFrame(WsOpcode::kClose, payload, true, nullptr);
}

} // namespace vinput::net
