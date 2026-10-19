#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <httplib.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/config/core_config_types.h"

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
  const std::string api_key = "test_key_xyz_123";

  CoreConfig config;
  config.asr.activeProvider = "provider.vinput.remote.streaming";
  CommandAsrProvider cmd;
  cmd.id = "provider.vinput.remote.streaming";
  cmd.env["VINPUT_ASR_PORT"] = "0"; // Ephemeral port
  cmd.env["VINPUT_ASR_API_KEY"] = api_key;
  cmd.env["VINPUT_ASR_DEBOUNCE_MS"] = "200";
  config.asr.providers.emplace_back(cmd);

  vinput::daemon::remote::RemoteTextService service;
  std::string err;
  TEST_ASSERT(service.Synchronize(config, &err));
  TEST_ASSERT(err.empty());

  const int test_port = service.Port();
  TEST_ASSERT(test_port > 0);

  const auto endpoints = service.ListEndpoints();
  (void)endpoints; // May be empty in loopback-only CI/container, but shouldn't crash

  httplib::Client cli("127.0.0.1", test_port);

  // 1. Test GET /health
  {
    auto res = cli.Get("/health");
    TEST_ASSERT(res && res->status == 200);
    TEST_ASSERT(res->get_header_value("Content-Type").find("application/json") !=
                std::string::npos);
    TEST_ASSERT(res->body.find("{\"ok\":true}") != std::string::npos);
  }

  // 2. Test GET /favicon.svg
  {
    auto res = cli.Get("/favicon.svg");
    TEST_ASSERT(res && res->status == 200);
    TEST_ASSERT(res->get_header_value("Content-Type").find("image/svg+xml") != std::string::npos);
    TEST_ASSERT(res->body.find("</svg>") != std::string::npos);
  }

  // 3. Test GET /
  {
    auto res = cli.Get("/");
    TEST_ASSERT(res && res->status == 200);
    TEST_ASSERT(res->get_header_value("Content-Type").find("text/html") != std::string::npos);
    TEST_ASSERT(res->body.find("</html>") != std::string::npos);
  }

  // 4. Test WebSocket /v1/realtime Unauthorized without Bearer
  {
    httplib::ws::WebSocketClient ws("ws://127.0.0.1:" + std::to_string(test_port) + "/v1/realtime");
    auto res = ws.connect();
    TEST_ASSERT(!res);
    TEST_ASSERT(res.status() == 401);
  }

  // 5. Test WebSocket /v1/realtime Authorized with Bearer
  const httplib::Headers rt_headers = {{"Authorization", "Bearer " + api_key}};
  httplib::ws::WebSocketClient rt_ws("ws://127.0.0.1:" + std::to_string(test_port) + "/v1/realtime",
                                     rt_headers);
  auto rt_conn = rt_ws.connect();
  TEST_ASSERT(bool(rt_conn));

  // 6. Test WebSocket /ws authentication and debounce text dispatch
  {
    httplib::ws::WebSocketClient ws("ws://127.0.0.1:" + std::to_string(test_port) + "/ws");
    TEST_ASSERT(bool(ws.connect()));

    // Send auth payload
    const std::string auth_json = std::string(R"({"type":"auth","api_key":")") + api_key + R"("})";
    TEST_ASSERT(ws.send(auth_json));

    std::string frame1;
    TEST_ASSERT(ws.read(frame1) == httplib::ws::ReadResult::Text);
    TEST_ASSERT(frame1.find("auth_ok") != std::string::npos);

    std::string frame2;
    TEST_ASSERT(ws.read(frame2) == httplib::ws::ReadResult::Text);
    TEST_ASSERT(frame2.find("init") != std::string::npos);

    // Send text update
    const std::string update_json = R"({"type":"text_update","text":"Hello from Remote"})";
    TEST_ASSERT(ws.send(update_json));

    // Realtime client should receive committed, delta, completed
    std::string rt_msg1;
    TEST_ASSERT(rt_ws.read(rt_msg1) == httplib::ws::ReadResult::Text);
    TEST_ASSERT(rt_msg1.find("input_audio_buffer.committed") != std::string::npos);

    std::string rt_msg2;
    TEST_ASSERT(rt_ws.read(rt_msg2) == httplib::ws::ReadResult::Text);
    TEST_ASSERT(rt_msg2.find("conversation.item.input_audio_transcription.delta") !=
                std::string::npos);
    TEST_ASSERT(rt_msg2.find("Hello from Remote") != std::string::npos);

    std::string rt_msg3;
    TEST_ASSERT(rt_ws.read(rt_msg3) == httplib::ws::ReadResult::Text);
    TEST_ASSERT(rt_msg3.find("conversation.item.input_audio_transcription.completed") !=
                std::string::npos);
    TEST_ASSERT(rt_msg3.find("Hello from Remote") != std::string::npos);

    ws.close();
  }

  rt_ws.close();

  // 7. Test Synchronize with inactive config shuts down service
  const CoreConfig empty_config;
  TEST_ASSERT(service.Synchronize(empty_config, &err));
  TEST_ASSERT(err.empty());

  service.Shutdown();
}

void TestPhoneDrivenDirectSession() {
  const std::string api_key = "direct_phone_key_456";

  CoreConfig config;
  config.asr.activeProvider = "provider.vinput.remote.streaming";
  CommandAsrProvider cmd;
  cmd.id = "provider.vinput.remote.streaming";
  cmd.env["VINPUT_ASR_PORT"] = "0"; // Ephemeral port
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

  const int test_port = service.Port();
  TEST_ASSERT(test_port > 0);

  httplib::ws::WebSocketClient ws("ws://127.0.0.1:" + std::to_string(test_port) + "/ws");
  TEST_ASSERT(bool(ws.connect()));

  // Authenticate
  const std::string auth_json = std::string(R"({"type":"auth","api_key":")") + api_key + R"("})";
  TEST_ASSERT(ws.send(auth_json));

  std::string frame1;
  TEST_ASSERT(ws.read(frame1) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame1.find("auth_ok") != std::string::npos);

  std::string frame2;
  TEST_ASSERT(ws.read(frame2) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame2.find("init") != std::string::npos);

  // 1. Phone sends text -> should trigger start_called!
  const std::string update_json = R"({"type":"text_update","text":"Mobile speaks"})";
  TEST_ASSERT(ws.send(update_json));
  TEST_ASSERT(WaitFor(start_called));

  // 2. Phone sends finalize -> should trigger stop_called and receive committed!
  const std::string finalize_json = R"({"type":"finalize"})";
  TEST_ASSERT(ws.send(finalize_json));
  TEST_ASSERT(WaitFor(stop_called));

  std::string frame3;
  TEST_ASSERT(ws.read(frame3) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame3.find("committed") != std::string::npos);
  TEST_ASSERT(service.GetText().empty());

  ws.close();
  service.Shutdown();
}

void TestInputSessionPreemption() {
  const std::string api_key = "preempt_key_789";

  CoreConfig config;
  config.asr.activeProvider = "provider.vinput.remote.streaming";
  CommandAsrProvider cmd;
  cmd.id = "provider.vinput.remote.streaming";
  cmd.env["VINPUT_ASR_PORT"] = "0"; // Ephemeral port
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

  const int test_port = service.Port();
  TEST_ASSERT(test_port > 0);

  const std::string auth_json = std::string(R"({"type":"auth","api_key":")") + api_key + R"("})";

  // 1. Client 1 connects and authenticates
  httplib::ws::WebSocketClient ws1("ws://127.0.0.1:" + std::to_string(test_port) + "/ws");
  TEST_ASSERT(bool(ws1.connect()));
  TEST_ASSERT(ws1.send(auth_json));

  std::string frame1_auth;
  TEST_ASSERT(ws1.read(frame1_auth) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame1_auth.find("auth_ok") != std::string::npos);

  std::string frame1_init;
  TEST_ASSERT(ws1.read(frame1_init) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame1_init.find("init") != std::string::npos);

  // Client 1 starts speaking -> triggers start_called
  const std::string client1_speech = R"({"type":"text_update","text":"Client 1 speaking"})";
  TEST_ASSERT(ws1.send(client1_speech));
  TEST_ASSERT(WaitFor(start_called));

  // 2. Client 2 connects with the same valid API key and authenticates -> preempts Client 1
  httplib::ws::WebSocketClient ws2("ws://127.0.0.1:" + std::to_string(test_port) + "/ws");
  TEST_ASSERT(bool(ws2.connect()));
  TEST_ASSERT(ws2.send(auth_json));

  // Client 2 should receive auth_ok and init
  std::string frame2_auth;
  TEST_ASSERT(ws2.read(frame2_auth) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame2_auth.find("auth_ok") != std::string::npos);

  std::string frame2_init;
  TEST_ASSERT(ws2.read(frame2_init) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame2_init.find("init") != std::string::npos);

  // Preemption should stop Client 1's ongoing recording
  TEST_ASSERT(WaitFor(stop_called));

  // 3. Client 1 should receive displaced event then close
  std::string frame1_displaced;
  TEST_ASSERT(ws1.read(frame1_displaced) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame1_displaced.find("displaced") != std::string::npos);

  // Next read on ws1 should fail because server closed it
  std::string frame1_closed;
  TEST_ASSERT(ws1.read(frame1_closed) == httplib::ws::ReadResult::Fail);

  // Reset tracking flags for Client 2
  start_called.store(false);
  stop_called.store(false);

  // 4. Client 2 can send input events normally
  const std::string update_json = R"({"type":"text_update","text":"Client 2 took over"})";
  TEST_ASSERT(ws2.send(update_json));
  TEST_ASSERT(WaitFor(start_called));

  const std::string finalize_json = R"({"type":"finalize"})";
  TEST_ASSERT(ws2.send(finalize_json));
  TEST_ASSERT(WaitFor(stop_called));

  std::string frame2_committed;
  TEST_ASSERT(ws2.read(frame2_committed) == httplib::ws::ReadResult::Text);
  TEST_ASSERT(frame2_committed.find("committed") != std::string::npos);
  TEST_ASSERT(service.GetText().empty());

  ws1.close();
  ws2.close();
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
