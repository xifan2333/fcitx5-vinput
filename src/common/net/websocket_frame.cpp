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

constexpr bool IsValidOpcode(std::uint8_t opcode) {
  switch (opcode) {
  case static_cast<std::uint8_t>(WsOpcode::kContinuation):
  case static_cast<std::uint8_t>(WsOpcode::kText):
  case static_cast<std::uint8_t>(WsOpcode::kBinary):
  case static_cast<std::uint8_t>(WsOpcode::kClose):
  case static_cast<std::uint8_t>(WsOpcode::kPing):
  case static_cast<std::uint8_t>(WsOpcode::kPong):
    return true;
  default:
    return false;
  }
}

constexpr bool IsControlOpcode(WsOpcode opcode) {
  return static_cast<std::uint8_t>(opcode) >= 0x8;
}

} // namespace

std::optional<WsFrameHeader> ParseWebSocketFrameHeader(std::string_view buffer) {
  if (buffer.size() < 2) {
    return std::nullopt;
  }

  const auto byte0 = static_cast<std::uint8_t>(buffer[0]);
  const auto byte1 = static_cast<std::uint8_t>(buffer[1]);

  const std::uint8_t opcode_val = byte0 & 0x0F;
  if (!IsValidOpcode(opcode_val)) {
    return std::nullopt;
  }

  WsFrameHeader header;
  header.fin = (byte0 & 0x80) != 0;
  header.opcode = static_cast<WsOpcode>(opcode_val);
  header.masked = (byte1 & 0x80) != 0;

  const std::uint8_t len_code = byte1 & 0x7F;
  std::size_t offset = 2;

  if (len_code < 126) {
    header.payload_length = len_code;
  } else if (len_code == 126) {
    if (buffer.size() < offset + 2) {
      return std::nullopt;
    }
    header.payload_length =
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[offset])) << 8) |
        static_cast<std::uint8_t>(buffer[offset + 1]);
    offset += 2;
  } else { // len_code == 127
    if (buffer.size() < offset + 8) {
      return std::nullopt;
    }
    std::uint64_t len = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      len = (len << 8) | static_cast<std::uint8_t>(buffer[offset + i]);
    }
    // RFC 6455 5.2: Most significant bit MUST be 0
    if ((len & (1ULL << 63)) != 0) {
      return std::nullopt;
    }
    header.payload_length = len;
    offset += 8;
  }

  // RFC 6455 5.5: Control frames MUST NOT be fragmented and MUST have payload length <= 125
  if (IsControlOpcode(header.opcode)) {
    if (!header.fin || header.payload_length > 125) {
      return std::nullopt;
    }
  }

  if (header.masked) {
    if (buffer.size() < offset + 4) {
      return std::nullopt;
    }
    std::memcpy(header.mask_key, buffer.data() + offset, 4);
    offset += 4;
  }

  header.header_size = offset;
  return header;
}

bool DecodeWebSocketFrame(std::string_view buffer, WsFrame* out_frame, std::size_t* consumed_bytes,
                          std::size_t max_payload_bytes, std::string* error) {
  if (consumed_bytes != nullptr) {
    *consumed_bytes = 0;
  }
  if (out_frame == nullptr || consumed_bytes == nullptr) {
    if (error != nullptr) {
      *error = "Invalid output pointer in DecodeWebSocketFrame";
    }
    return false;
  }

  const auto header_opt = ParseWebSocketFrameHeader(buffer);
  if (!header_opt) {
    if (buffer.size() < 2) {
      if (error != nullptr) {
        *error = "Incomplete frame header: buffer too short";
      }
      return false;
    }
    const auto byte0 = static_cast<std::uint8_t>(buffer[0]);
    const auto byte1 = static_cast<std::uint8_t>(buffer[1]);
    const std::uint8_t opcode_val = byte0 & 0x0F;
    if (!IsValidOpcode(opcode_val)) {
      if (error != nullptr) {
        *error = "Invalid WebSocket opcode";
      }
      return false;
    }
    const std::uint8_t len_code = byte1 & 0x7F;
    const bool masked = (byte1 & 0x80) != 0;
    std::size_t ext_len_bytes = 0;
    if (len_code == 126) {
      ext_len_bytes = 2;
    } else if (len_code == 127) {
      ext_len_bytes = 8;
    }
    const std::size_t required_header_size = 2 + ext_len_bytes + (masked ? 4 : 0);
    if (buffer.size() < required_header_size) {
      if (error != nullptr) {
        *error = "Incomplete frame header: waiting for extended length or mask";
      }
      return false;
    }
    if (error != nullptr) {
      *error = "Malformed WebSocket frame header";
    }
    return false;
  }

  const WsFrameHeader& header = *header_opt;
  if (header.payload_length > max_payload_bytes) {
    if (error != nullptr) {
      *error = "WebSocket frame payload exceeds maximum allowed size";
    }
    return false;
  }

  if (buffer.size() < header.header_size + header.payload_length) {
    if (error != nullptr) {
      *error = "Incomplete frame payload";
    }
    return false;
  }

  std::string payload(buffer.substr(header.header_size, header.payload_length));
  if (header.masked) {
    ApplyWebSocketMask(payload.data(), payload.size(), header.mask_key, 0);
  }

  out_frame->header = header;
  out_frame->payload = std::move(payload);
  *consumed_bytes = header.header_size + header.payload_length;
  return true;
}

std::string EncodeWebSocketFrame(WsOpcode opcode, std::string_view payload, bool fin,
                                 const std::uint8_t* mask_key) {
  std::string frame;
  const std::size_t payload_len = payload.size();
  std::size_t reserve_size = 2 + payload_len;
  if (payload_len >= 126 && payload_len <= 0xFFFF) {
    reserve_size += 2;
  } else if (payload_len > 0xFFFF) {
    reserve_size += 8;
  }
  if (mask_key != nullptr) {
    reserve_size += 4;
  }
  frame.reserve(reserve_size);

  const auto byte0 =
      static_cast<char>((fin ? 0x80 : 0x00) | (static_cast<std::uint8_t>(opcode) & 0x0F));
  frame.push_back(byte0);

  const std::uint8_t mask_flag = (mask_key != nullptr) ? 0x80 : 0x00;
  if (payload_len < 126) {
    frame.push_back(static_cast<char>(mask_flag | static_cast<std::uint8_t>(payload_len)));
  } else if (payload_len <= 0xFFFF) {
    frame.push_back(static_cast<char>(mask_flag | 126));
    frame.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(payload_len & 0xFF));
  } else {
    frame.push_back(static_cast<char>(mask_flag | 127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload_len) >> shift) & 0xFF));
    }
  }

  if (mask_key != nullptr) {
    frame.append(reinterpret_cast<const char*>(mask_key), 4);
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
  return EncodeWebSocketFrame(WsOpcode::kPing, payload, true, nullptr);
}

std::string EncodeWebSocketPong(std::string_view payload) {
  return EncodeWebSocketFrame(WsOpcode::kPong, payload, true, nullptr);
}

std::string EncodeWebSocketClose(std::uint16_t status_code, std::string_view reason) {
  std::string payload;
  if (status_code != 0 || !reason.empty()) {
    payload.reserve(2 + reason.size());
    payload.push_back(static_cast<char>((status_code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(status_code & 0xFF));
    payload.append(reason);
  }
  return EncodeWebSocketFrame(WsOpcode::kClose, payload, true, nullptr);
}

} // namespace vinput::net
