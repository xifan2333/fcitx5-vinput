#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/net/websocket_frame.h"

#include "daemon/remote/remote_assets.h"
#include "daemon/remote/remote_text_service.h"

namespace {

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

std::uint16_t FindFreePort() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  TEST_ASSERT(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  sockaddr sa{};
  std::memcpy(&sa, &addr, sizeof(addr));
  TEST_ASSERT(bind(fd, &sa, sizeof(addr)) == 0);
  socklen_t len = sizeof(sa);
  TEST_ASSERT(getsockname(fd, &sa, &len) == 0);
  std::memcpy(&addr, &sa, sizeof(addr));
  const auto port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

int ConnectToPort(std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  sockaddr sa{};
  std::memcpy(&sa, &addr, sizeof(addr));
  if (connect(fd, &sa, sizeof(addr)) != 0) {
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
  char buf[512];
  while (result.find(delimiter) == std::string::npos) {
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
      break;
    }
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    result.append(buf, static_cast<std::size_t>(n));
  }
  return result;
}

bool RecvFrame(int fd, std::string* inout_buffer, vinput::net::WsFrame* out_frame,
               int timeout_ms = 3000) {
  while (true) {
    if (!inout_buffer->empty()) {
      std::size_t consumed = 0;
      std::string err;
      const auto res = vinput::net::DecodeWebSocketFrame(*inout_buffer, out_frame, &consumed,
                                                         1024ULL * 1024ULL, &err);
      if (res == vinput::net::WsDecodeStatus::kOk) {
        inout_buffer->erase(0, consumed);
        return true;
      }
      if (res == vinput::net::WsDecodeStatus::kProtocolError) {
        return false;
      }
    }

    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
      return false;
    }
    char chunk[512];
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return false;
    }
    inout_buffer->append(chunk, static_cast<std::size_t>(n));
  }
}

bool WaitFor(const std::atomic<bool>& flag, int timeout_ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return flag.load();
}

void TestRemoteAssets() {
  const std::string html = vinput::daemon::remote::LoadRemoteIndexHtml();
  TEST_ASSERT(!html.empty());
  TEST_ASSERT(html.find("<!doctype html>") != std::string::npos);
  TEST_ASSERT(html.find("VInput Remote") != std::string::npos);
  TEST_ASSERT(html.find("enterkeyhint=\"send\"") != std::string::npos);

  const std::string svg = vinput::daemon::remote::LoadRemoteFaviconSvg();
  TEST_ASSERT(!svg.empty());
  TEST_ASSERT(svg.find("<svg") != std::string::npos);
  TEST_ASSERT(svg.find("</svg>") != std::string::npos);
}

void TestRemoteTextServiceHttpAndWs() {
  const std::uint16_t port = FindFreePort();
  const std::string api_key = "test_key_xyz_123";

  CoreConfig config;
  config.asr.activeProvider = "provider.vinput.remote.streaming";
  CommandAsrProvider cmd;
  cmd.id = "provider.vinput.remote.streaming";
  cmd.env["VINPUT_ASR_PORT"] = std::to_string(port);
  cmd.env["VINPUT_ASR_API_KEY"] = api_key;
  cmd.env["VINPUT_ASR_DEBOUNCE_MS"] = "200";
  config.asr.providers.emplace_back(cmd);

  vinput::daemon::remote::RemoteTextService service;
  std::string err;
  TEST_ASSERT(service.Synchronize(config, &err));
  TEST_ASSERT(err.empty());

  // 1. Test GET /health
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "{\"ok\":true}");
    TEST_ASSERT(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    TEST_ASSERT(resp.find("application/json") != std::string::npos);
    close(fd);
  }

  // 2. Test GET /favicon.svg
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "GET /favicon.svg HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "</svg>");
    TEST_ASSERT(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    TEST_ASSERT(resp.find("image/svg+xml") != std::string::npos);
    close(fd);
  }

  // 3. Test GET /
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(SendRaw(fd, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const std::string resp = RecvUntil(fd, "</html>");
    TEST_ASSERT(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    TEST_ASSERT(resp.find("text/html") != std::string::npos);
    close(fd);
  }

  // 4. Test WebSocket /v1/realtime Unauthorized without Bearer
  {
    const int fd = ConnectToPort(port);
    TEST_ASSERT(fd >= 0);
    const std::string req = "GET /v1/realtime HTTP/1.1\r\n"
                            "Host: 127.0.0.1\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n";
    TEST_ASSERT(SendRaw(fd, req));
    const std::string resp = RecvUntil(fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 401 Unauthorized") != std::string::npos);
    close(fd);
  }

  // 5. Test WebSocket /v1/realtime Authorized with Bearer
  const int realtime_fd = ConnectToPort(port);
  TEST_ASSERT(realtime_fd >= 0);
  std::string rt_buf;
  {
    const std::string req = "GET /v1/realtime HTTP/1.1\r\n"
                            "Host: 127.0.0.1\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "Authorization: Bearer " +
                            api_key + "\r\n\r\n";
    TEST_ASSERT(SendRaw(realtime_fd, req));
    const std::string resp = RecvUntil(realtime_fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
  }

  // 6. Test WebSocket /ws authentication and debounce text dispatch
  {
    const int ws_fd = ConnectToPort(port);
    TEST_ASSERT(ws_fd >= 0);
    std::string ws_buf;
    const std::string req = "GET /ws HTTP/1.1\r\n"
                            "Host: 127.0.0.1\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n";
    TEST_ASSERT(SendRaw(ws_fd, req));
    const std::string resp = RecvUntil(ws_fd, "\r\n\r\n");
    TEST_ASSERT(resp.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);

    // Send auth payload
    const std::uint8_t mask1[4] = {0x11, 0x22, 0x33, 0x44};
    const std::string auth_json = std::string(R"({"type":"auth","api_key":")") + api_key + R"("})";
    TEST_ASSERT(SendRaw(ws_fd, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                                 auth_json, true, mask1)));

    vinput::net::WsFrame frame1;
    TEST_ASSERT(RecvFrame(ws_fd, &ws_buf, &frame1));
    TEST_ASSERT(frame1.payload.find("auth_ok") != std::string::npos);

    vinput::net::WsFrame frame2;
    TEST_ASSERT(RecvFrame(ws_fd, &ws_buf, &frame2));
    TEST_ASSERT(frame2.payload.find("init") != std::string::npos);

    // Send text update
    const std::string update_json = R"({"type":"text_update","text":"Hello from Remote"})";
    TEST_ASSERT(SendRaw(ws_fd, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                                 update_json, true, mask1)));

    // Realtime client should receive committed, delta, completed
    vinput::net::WsFrame rt_frame1;
    TEST_ASSERT(RecvFrame(realtime_fd, &rt_buf, &rt_frame1));
    TEST_ASSERT(rt_frame1.payload.find("input_audio_buffer.committed") != std::string::npos);

    vinput::net::WsFrame rt_frame2;
    TEST_ASSERT(RecvFrame(realtime_fd, &rt_buf, &rt_frame2));
    TEST_ASSERT(rt_frame2.payload.find("conversation.item.input_audio_transcription.delta") !=
                std::string::npos);
    TEST_ASSERT(rt_frame2.payload.find("Hello from Remote") != std::string::npos);

    vinput::net::WsFrame rt_frame3;
    TEST_ASSERT(RecvFrame(realtime_fd, &rt_buf, &rt_frame3));
    TEST_ASSERT(rt_frame3.payload.find("conversation.item.input_audio_transcription.completed") !=
                std::string::npos);
    TEST_ASSERT(rt_frame3.payload.find("Hello from Remote") != std::string::npos);

    close(ws_fd);
  }

  close(realtime_fd);

  // 7. Test ListEndpoints
  const auto endpoints = service.ListEndpoints();
  (void)endpoints; // May be empty in loopback-only CI/container, but shouldn't crash

  // 8. Test Synchronize with inactive config shuts down service
  const CoreConfig empty_config;
  TEST_ASSERT(service.Synchronize(empty_config, &err));
  TEST_ASSERT(err.empty());

  service.Shutdown();
}

void TestPhoneDrivenDirectSession() {
  const std::uint16_t port = FindFreePort();
  const std::string api_key = "direct_phone_key_456";

  CoreConfig config;
  config.asr.activeProvider = "provider.vinput.remote.streaming";
  CommandAsrProvider cmd;
  cmd.id = "provider.vinput.remote.streaming";
  cmd.env["VINPUT_ASR_PORT"] = std::to_string(port);
  cmd.env["VINPUT_ASR_API_KEY"] = api_key;
  cmd.env["VINPUT_ASR_DEBOUNCE_MS"] = "200";
  config.asr.providers.emplace_back(cmd);

  vinput::daemon::remote::RemoteTextService service;
  std::atomic<bool> start_called{false};
  std::atomic<bool> stop_called{false};
  service.SetSessionControlCallbacks(
      [&start_called]() {
        start_called.store(true);
        return true;
      },
      [&stop_called](const std::string&) {
        stop_called.store(true);
        return true;
      });

  std::string err;
  TEST_ASSERT(service.Synchronize(config, &err));
  TEST_ASSERT(err.empty());

  const int ws_fd = ConnectToPort(port);
  TEST_ASSERT(ws_fd >= 0);
  std::string ws_buf;
  const std::string req = "GET /ws HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\n\r\n";
  TEST_ASSERT(SendRaw(ws_fd, req));
  const std::string resp = RecvUntil(ws_fd, "\r\n\r\n");
  TEST_ASSERT(resp.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);

  // Authenticate
  const std::uint8_t mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  const std::string auth_json = std::string(R"({"type":"auth","api_key":")") + api_key + R"("})";
  TEST_ASSERT(SendRaw(ws_fd, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                               auth_json, true, mask)));

  vinput::net::WsFrame frame1;
  TEST_ASSERT(RecvFrame(ws_fd, &ws_buf, &frame1));
  TEST_ASSERT(frame1.payload.find("auth_ok") != std::string::npos);

  vinput::net::WsFrame frame2;
  TEST_ASSERT(RecvFrame(ws_fd, &ws_buf, &frame2));
  TEST_ASSERT(frame2.payload.find("init") != std::string::npos);

  // 1. Phone sends text -> should trigger start_called!
  const std::string update_json = R"({"type":"text_update","text":"Mobile speaks"})";
  TEST_ASSERT(SendRaw(ws_fd, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                               update_json, true, mask)));
  TEST_ASSERT(WaitFor(start_called));

  // 2. Phone sends finalize -> should trigger stop_called and receive committed!
  const std::string finalize_json = R"({"type":"finalize"})";
  TEST_ASSERT(SendRaw(ws_fd, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                               finalize_json, true, mask)));
  TEST_ASSERT(WaitFor(stop_called));

  vinput::net::WsFrame frame3;
  TEST_ASSERT(RecvFrame(ws_fd, &ws_buf, &frame3));
  TEST_ASSERT(frame3.payload.find("committed") != std::string::npos);
  TEST_ASSERT(service.GetText().empty());

  close(ws_fd);
  service.Shutdown();
}

void TestInputSessionPreemption() {
  const std::uint16_t port = FindFreePort();
  const std::string api_key = "preempt_key_789";

  CoreConfig config;
  config.asr.activeProvider = "provider.vinput.remote.streaming";
  CommandAsrProvider cmd;
  cmd.id = "provider.vinput.remote.streaming";
  cmd.env["VINPUT_ASR_PORT"] = std::to_string(port);
  cmd.env["VINPUT_ASR_API_KEY"] = api_key;
  cmd.env["VINPUT_ASR_DEBOUNCE_MS"] = "200";
  config.asr.providers.emplace_back(cmd);

  vinput::daemon::remote::RemoteTextService service;
  std::atomic<bool> start_called{false};
  std::atomic<bool> stop_called{false};
  service.SetSessionControlCallbacks(
      [&start_called]() {
        start_called.store(true);
        return true;
      },
      [&stop_called](const std::string&) {
        stop_called.store(true);
        return true;
      });

  std::string err;
  TEST_ASSERT(service.Synchronize(config, &err));
  TEST_ASSERT(err.empty());

  const std::string ws_handshake = "GET /ws HTTP/1.1\r\n"
                                   "Host: 127.0.0.1\r\n"
                                   "Upgrade: websocket\r\n"
                                   "Connection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                   "Sec-WebSocket-Version: 13\r\n\r\n";
  const std::uint8_t mask1[4] = {0x11, 0x22, 0x33, 0x44};
  const std::uint8_t mask2[4] = {0x55, 0x66, 0x77, 0x88};
  const std::string auth_json = std::string(R"({"type":"auth","api_key":")") + api_key + R"("})";

  // 1. Client 1 connects and authenticates
  const int ws_fd1 = ConnectToPort(port);
  TEST_ASSERT(ws_fd1 >= 0);
  std::string ws_buf1;
  TEST_ASSERT(SendRaw(ws_fd1, ws_handshake));
  const std::string resp1 = RecvUntil(ws_fd1, "\r\n\r\n");
  TEST_ASSERT(resp1.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);

  TEST_ASSERT(SendRaw(ws_fd1, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                                auth_json, true, mask1)));
  vinput::net::WsFrame frame1_auth;
  TEST_ASSERT(RecvFrame(ws_fd1, &ws_buf1, &frame1_auth));
  TEST_ASSERT(frame1_auth.payload.find("auth_ok") != std::string::npos);

  vinput::net::WsFrame frame1_init;
  TEST_ASSERT(RecvFrame(ws_fd1, &ws_buf1, &frame1_init));
  TEST_ASSERT(frame1_init.payload.find("init") != std::string::npos);

  // Client 1 starts speaking -> triggers start_called
  const std::string client1_speech = R"({"type":"text_update","text":"Client 1 speaking"})";
  TEST_ASSERT(SendRaw(ws_fd1, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                                client1_speech, true, mask1)));
  TEST_ASSERT(WaitFor(start_called));

  // 2. Client 2 connects with the same valid API key and authenticates -> preempts Client 1
  const int ws_fd2 = ConnectToPort(port);
  TEST_ASSERT(ws_fd2 >= 0);
  std::string ws_buf2;
  TEST_ASSERT(SendRaw(ws_fd2, ws_handshake));
  const std::string resp2 = RecvUntil(ws_fd2, "\r\n\r\n");
  TEST_ASSERT(resp2.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);

  TEST_ASSERT(SendRaw(ws_fd2, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                                auth_json, true, mask2)));

  // Client 2 should receive auth_ok and init
  vinput::net::WsFrame frame2_auth;
  TEST_ASSERT(RecvFrame(ws_fd2, &ws_buf2, &frame2_auth));
  TEST_ASSERT(frame2_auth.payload.find("auth_ok") != std::string::npos);

  vinput::net::WsFrame frame2_init;
  TEST_ASSERT(RecvFrame(ws_fd2, &ws_buf2, &frame2_init));
  TEST_ASSERT(frame2_init.payload.find("init") != std::string::npos);

  // Preemption should stop Client 1's ongoing recording
  TEST_ASSERT(WaitFor(stop_called));

  // 3. Client 1 should receive displaced event then Close frame with status 1000
  vinput::net::WsFrame frame1_displaced;
  TEST_ASSERT(RecvFrame(ws_fd1, &ws_buf1, &frame1_displaced));
  TEST_ASSERT(frame1_displaced.payload.find("displaced") != std::string::npos);

  vinput::net::WsFrame close_frame1;
  TEST_ASSERT(RecvFrame(ws_fd1, &ws_buf1, &close_frame1));
  TEST_ASSERT(close_frame1.header.opcode == vinput::net::WsOpcode::kClose);
  TEST_ASSERT(close_frame1.payload.size() >= 2);
  const auto close_code =
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(close_frame1.payload[0])) << 8) |
      static_cast<std::uint8_t>(close_frame1.payload[1]);
  TEST_ASSERT(close_code == 1000);
  TEST_ASSERT(close_frame1.payload.substr(2) == "Replaced by new authenticated session.");

  // Reset tracking flags for Client 2
  start_called.store(false);
  stop_called.store(false);

  // 4. Client 2 can send input events normally
  const std::string update_json = R"({"type":"text_update","text":"Client 2 took over"})";
  TEST_ASSERT(SendRaw(ws_fd2, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                                update_json, true, mask2)));
  TEST_ASSERT(WaitFor(start_called));

  const std::string finalize_json = R"({"type":"finalize"})";
  TEST_ASSERT(SendRaw(ws_fd2, vinput::net::EncodeWebSocketFrame(vinput::net::WsOpcode::kText,
                                                                finalize_json, true, mask2)));
  TEST_ASSERT(WaitFor(stop_called));

  vinput::net::WsFrame frame2_committed;
  TEST_ASSERT(RecvFrame(ws_fd2, &ws_buf2, &frame2_committed));
  TEST_ASSERT(frame2_committed.payload.find("committed") != std::string::npos);
  TEST_ASSERT(service.GetText().empty());

  close(ws_fd1);
  close(ws_fd2);
  service.Shutdown();
}

} // namespace

int main() {
  TestRemoteAssets();
  TestRemoteTextServiceHttpAndWs();
  TestPhoneDrivenDirectSession();
  TestInputSessionPreemption();
  std::puts("All remote text service tests passed!");
  return 0;
}
