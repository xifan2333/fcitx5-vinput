#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

#include "common/net/http_server.h"
#include "common/net/http_types.h"
#include "common/net/websocket_frame.h"
#include "common/utils/string_utils.h"

namespace {

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

int ConnectToPort(std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  sockaddr sa_buf[2]{};
  std::memcpy(&sa_buf[0], &addr, sizeof(addr));
  if (connect(fd, &sa_buf[0], sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool SendRaw(int fd, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = send(fd, data.data() + offset, data.size() - offset, 0);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

std::string RecvUntil(int fd, std::string_view delimiter, int timeout_ms = 3000) {
  std::string result;
  char buf[1024];
  const auto start = std::chrono::steady_clock::now();
  while (result.find(delimiter) == std::string::npos) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (elapsed > timeout_ms) {
      break;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int ret = poll(&pfd, 1, 100);
    if (ret > 0 && (pfd.revents & POLLIN) != 0) {
      const ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n > 0) {
        result.append(buf, static_cast<std::size_t>(n));
      } else if (n == 0 || (n < 0 && errno != EINTR)) {
        break;
      }
    }
  }
  return result;
}

bool RecvFrame(int fd, vinput::net::WsFrame* out_frame, int timeout_ms = 3000) {
  std::string buffer;
  char buf[1024];
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    std::size_t consumed = 0;
    std::string err;
    const auto status =
        vinput::net::DecodeWebSocketFrame(buffer, out_frame, &consumed, 1024ULL * 1024ULL, &err);
    if (status == vinput::net::WsDecodeStatus::kOk) {
      return true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (elapsed > timeout_ms) {
      return false;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int ret = poll(&pfd, 1, 100);
    if (ret > 0 && (pfd.revents & POLLIN) != 0) {
      const ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n > 0) {
        buffer.append(buf, static_cast<std::size_t>(n));
      } else if (n == 0 || (n < 0 && errno != EINTR)) {
        return false;
      }
    }
  }
}

void TestHttpRouting() {
  vinput::net::HttpServerConfig config;
  config.port = 0;
  vinput::net::HttpServer server(config);

  server.Get("/ping", [](const vinput::net::HttpRequest&) {
    vinput::net::HttpResponse res;
    res.status_code = 200;
    res.body = "pong";
    return res;
  });

  server.Post("/echo", [](const vinput::net::HttpRequest& req) {
    vinput::net::HttpResponse res;
    res.status_code = 200;
    res.body = req.body;
    return res;
  });

  server.Get("/nocontent", [](const vinput::net::HttpRequest&) {
    vinput::net::HttpResponse res;
    res.status_code = 204;
    res.body = "should_be_suppressed";
    return res;
  });

  TEST_ASSERT(server.Start());
  const std::uint16_t port = server.BoundPort();
  TEST_ASSERT(port > 0);

  // 1. GET /ping
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "pong");
    TEST_ASSERT(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    TEST_ASSERT(resp.find("pong") != std::string::npos);
    close(fd);
  }

  // 2. POST /echo
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    const std::string payload = "hello lightweight http server";
    const std::string req = "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                            std::to_string(payload.size()) + "\r\n\r\n" + payload;
    TEST_ASSERT(SendRaw(fd, req));
    const std::string resp = RecvUntil(fd, payload);
    TEST_ASSERT(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    TEST_ASSERT(resp.find(payload) != std::string::npos);
    close(fd);
  }

  // 3. GET /ping?foo=bar (query parameters preserve route matching)
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "GET /ping?foo=bar&num=42 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "pong");
    TEST_ASSERT(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    TEST_ASSERT(resp.find("pong") != std::string::npos);
    close(fd);
  }

  // 4. GET /nocontent (204 No Content suppresses body and Content-Length)
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "GET /nocontent HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 204 No Content") != std::string::npos);
    TEST_ASSERT(resp.find("Content-Length") == std::string::npos);
    TEST_ASSERT(resp.find("should_be_suppressed") == std::string::npos);
    close(fd);
  }

  server.Stop();
}

void TestHttpErrors() {
  vinput::net::HttpServerConfig config;
  config.port = 0;
  config.max_request_body_bytes = 16;
  vinput::net::HttpServer server(config);

  server.Get("/ping", [](const vinput::net::HttpRequest&) {
    vinput::net::HttpResponse res;
    res.status_code = 200;
    res.body = "pong";
    return res;
  });

  TEST_ASSERT(server.Start());
  const std::uint16_t port = server.BoundPort();

  // 1. 404 Not Found
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "GET /notfound HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 404 Not Found") != std::string::npos);
    close(fd);
  }

  // 2. 405 Method Not Allowed (POST to GET endpoint)
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "POST /ping HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 405 Method Not Allowed") != std::string::npos);
    TEST_ASSERT(resp.find("Allow: GET") != std::string::npos);
    close(fd);
  }

  // 3. 400 Bad Request
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "INVALID_REQUEST_LINE\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 400 Bad Request") != std::string::npos);
    close(fd);
  }

  // 4. 413 Payload Too Large
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    const std::string big_payload(64, 'x');
    const std::string req = "POST /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                            std::to_string(big_payload.size()) + "\r\n\r\n" + big_payload;
    TEST_ASSERT(SendRaw(fd, req));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 413 Payload Too Large") != std::string::npos);
    close(fd);
  }

  // 5. Malformed Content-Length (strictly numeric)
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    const std::string req =
        "POST /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10junk\r\n\r\n12345";
    TEST_ASSERT(SendRaw(fd, req));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 400 Bad Request") != std::string::npos);
    close(fd);
  }

  server.Stop();
}

void TestWebSocketUpgradeAndDuplex() {
  vinput::net::HttpServerConfig config;
  config.port = 0;
  vinput::net::HttpServer server(config);

  std::shared_ptr<vinput::net::WebSocketSession> active_session;

  server.UpgradeWebSocket(
      "/ws",
      [&active_session](std::shared_ptr<vinput::net::WebSocketSession> session) {
        active_session = std::move(session);
      },
      [&active_session](std::string_view msg, bool is_binary) {
        if (!is_binary && msg == "hello") {
          active_session->SendText("world");
        } else if (is_binary) {
          active_session->SendBinary(msg);
        }
      });

  TEST_ASSERT(server.Start());
  const std::uint16_t port = server.BoundPort();

  const int fd = ConnectToPort(port);
  TEST_ASSERT(fd >= 0);

  // Send RFC 6455 Handshake
  const std::string handshake = "GET /ws HTTP/1.1\r\n"
                                "Host: 127.0.0.1\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n";
  TEST_ASSERT(SendRaw(fd, handshake));

  const std::string hs_resp = RecvUntil(fd, "\r\n\r\n");
  TEST_ASSERT(hs_resp.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
  TEST_ASSERT(hs_resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
              std::string::npos);

  // 1. Text frame exchange (Client to Server must be masked)
  const std::uint8_t mask1[4] = {0x12, 0x34, 0x56, 0x78};
  const std::string client_txt =
      vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText, "hello", true, mask1);
  TEST_ASSERT(SendRaw(fd, client_txt));

  vinput::net::WsFrame resp_frame;
  TEST_ASSERT(RecvFrame(fd, &resp_frame));
  TEST_ASSERT(resp_frame.header.opcode == vinput::net::WsOpcode::kText);
  TEST_ASSERT(resp_frame.payload == "world");

  // 2. Binary frame exchange
  const std::uint8_t mask2[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  const std::string bin_payload = "\x01\x02\x03\x04\xFF";
  const std::string client_bin =
      vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kBinary, bin_payload, true, mask2);
  TEST_ASSERT(SendRaw(fd, client_bin));

  TEST_ASSERT(RecvFrame(fd, &resp_frame));
  TEST_ASSERT(resp_frame.header.opcode == vinput::net::WsOpcode::kBinary);
  TEST_ASSERT(resp_frame.payload == bin_payload);

  // 3. Ping -> Pong automatic reply
  const std::uint8_t mask3[4] = {0x01, 0x02, 0x03, 0x04};
  const std::string client_ping =
      vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kPing, "ping_data", true, mask3);
  TEST_ASSERT(SendRaw(fd, client_ping));

  TEST_ASSERT(RecvFrame(fd, &resp_frame));
  TEST_ASSERT(resp_frame.header.opcode == vinput::net::WsOpcode::kPong);
  TEST_ASSERT(resp_frame.payload == "ping_data");

  // 4. SendJson test
  active_session->SendJson(R"({"status":"ok","count":42})");
  TEST_ASSERT(RecvFrame(fd, &resp_frame));
  TEST_ASSERT(resp_frame.header.opcode == vinput::net::WsOpcode::kText);
  TEST_ASSERT(resp_frame.payload == R"({"status":"ok","count":42})");

  // 5. Unmasked client frame must be rejected with 1002 (RFC 6455 5.1)
  const std::string unmasked_frame =
      vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText, "unmasked", true, nullptr);
  TEST_ASSERT(SendRaw(fd, unmasked_frame));
  TEST_ASSERT(RecvFrame(fd, &resp_frame));
  TEST_ASSERT(resp_frame.header.opcode == vinput::net::WsOpcode::kClose);
  TEST_ASSERT(resp_frame.payload.size() >= 2);
  const auto close_code = (static_cast<std::uint8_t>(resp_frame.payload[0]) << 8) |
                          static_cast<std::uint8_t>(resp_frame.payload[1]);
  TEST_ASSERT(close_code == 1002);

  close(fd);
  server.Stop();
}

void TestWebSocketAuthorizer() {
  vinput::net::HttpServerConfig config;
  config.port = 0;
  vinput::net::HttpServer server(config);

  server.UpgradeWebSocket(
      "/secure",
      [](const std::shared_ptr<vinput::net::WebSocketSession>&, const vinput::net::HttpRequest&) {},
      [](const vinput::net::HttpRequest& req) -> std::optional<vinput::net::HttpResponse> {
        for (const auto& h : req.headers) {
          if (vinput::str::EqualsIgnoreCase(h.name, "Authorization") &&
              h.value == "Bearer secret_token_123") {
            return std::nullopt; // Authorized!
          }
        }
        vinput::net::HttpResponse unauth;
        unauth.status_code = 401;
        unauth.body = "Unauthorized\n";
        return unauth;
      });

  TEST_ASSERT(server.Start());
  const std::uint16_t port = server.BoundPort();

  // 1. Missing Authorization header -> 401
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    const std::string hs = "GET /secure HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n";
    TEST_ASSERT(SendRaw(fd, hs));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 401 Unauthorized") != std::string::npos);
    close(fd);
  }

  // 2. Valid Authorization header -> 101 Switching Protocols
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    const std::string hs = "GET /secure HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Authorization: Bearer secret_token_123\r\n"
                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n";
    TEST_ASSERT(SendRaw(fd, hs));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
    close(fd);
  }

  server.Stop();
}

void TestServerLifecycle() {
  vinput::net::HttpServerConfig config;
  config.port = 0;
  vinput::net::HttpServer server(config);

  TEST_ASSERT(!server.IsRunning());
  TEST_ASSERT(server.Start());
  TEST_ASSERT(server.IsRunning());
  TEST_ASSERT(!server.Start()); // Already running
  TEST_ASSERT(server.BoundPort() > 0);

  server.Stop();
  TEST_ASSERT(!server.IsRunning());
}

} // namespace

int main() {
  std::puts("Running TestHttpRouting...");
  TestHttpRouting();

  std::puts("Running TestHttpErrors...");
  TestHttpErrors();

  std::puts("Running TestWebSocketUpgradeAndDuplex...");
  TestWebSocketUpgradeAndDuplex();

  std::puts("Running TestWebSocketAuthorizer...");
  TestWebSocketAuthorizer();

  std::puts("Running TestServerLifecycle...");
  TestServerLifecycle();

  std::puts("All HttpServer and WebSocket tests passed successfully!");
  return 0;
}
