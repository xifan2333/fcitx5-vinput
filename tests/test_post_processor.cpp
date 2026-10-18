#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"

#include "daemon/postprocess/post_processor.h"

namespace {

void AssertTrue(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

class MockHttpServer {
public:
  MockHttpServer() : listen_fd_(socket(AF_INET, SOCK_STREAM, 0)) {
    AssertTrue(listen_fd_ >= 0, "create listening socket");
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr sa{};
    std::memcpy(&sa, &sin, sizeof(sin));
    AssertTrue(bind(listen_fd_, &sa, sizeof(sin)) == 0, "bind socket to loopback");
    socklen_t len = sizeof(sa);
    AssertTrue(getsockname(listen_fd_, &sa, &len) == 0, "getsockname");
    std::memcpy(&sin, &sa, sizeof(sin));
    port_ = ntohs(sin.sin_port);
    AssertTrue(listen(listen_fd_, 8) == 0, "listen on socket");
    worker_thread_ = std::thread([this] { RunServerLoop(); });
  }

  MockHttpServer(const MockHttpServer&) = delete;
  MockHttpServer& operator=(const MockHttpServer&) = delete;
  MockHttpServer(MockHttpServer&&) = delete;
  MockHttpServer& operator=(MockHttpServer&&) = delete;

  ~MockHttpServer() {
    Stop();
    close(listen_fd_);
  }

  void Stop() {
    stop_requested_.store(true);
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }

  [[nodiscard]] std::uint16_t Port() const { return port_; }

  [[nodiscard]] std::vector<std::string> CapturedBodies() const {
    const std::lock_guard lock(mutex_);
    return captured_bodies_;
  }

  [[nodiscard]] std::vector<std::string> CapturedHeaders() const {
    const std::lock_guard lock(mutex_);
    return captured_headers_;
  }

  [[nodiscard]] int RequestCount() const { return request_count_.load(); }

private:
  bool ReadHttpRequest(int client_fd, std::string& out_body, std::string& out_header) const {
    std::string buffer;
    while (!stop_requested_.load()) {
      const std::size_t head_end = buffer.find("\r\n\r\n");
      if (head_end != std::string::npos) {
        const std::string length_key = "Content-Length: ";
        const auto len_pos = buffer.find(length_key);
        const std::size_t body_size = len_pos == std::string::npos
                                          ? 0
                                          : std::stoul(buffer.substr(len_pos + length_key.size()));
        if (buffer.size() >= head_end + 4 + body_size) {
          out_header = buffer.substr(0, head_end);
          out_body = buffer.substr(head_end + 4, body_size);
          return true;
        }
      }
      pollfd pfd{.fd = client_fd, .events = POLLIN, .revents = 0};
      if (poll(&pfd, 1, 100) <= 0) {
        continue;
      }
      char chunk[4096];
      const ssize_t bytes_read = recv(client_fd, chunk, sizeof(chunk), 0);
      if (bytes_read <= 0) {
        return false;
      }
      buffer.append(chunk, static_cast<std::size_t>(bytes_read));
    }
    return false;
  }

  void RunServerLoop() {
    while (!stop_requested_.load()) {
      pollfd pfd{.fd = listen_fd_, .events = POLLIN, .revents = 0};
      if (poll(&pfd, 1, 100) <= 0) {
        continue;
      }
      const int client_fd = accept(listen_fd_, nullptr, nullptr);
      if (client_fd < 0) {
        continue;
      }

      while (!stop_requested_.load()) {
        std::string request_body;
        std::string request_header;
        if (!ReadHttpRequest(client_fd, request_body, request_header)) {
          break;
        }

        {
          const std::lock_guard lock(mutex_);
          captured_headers_.push_back(request_header);
          captured_bodies_.push_back(request_body);
        }
        ++request_count_;

        std::string response;
        if (request_header.find("POST /error500/") != std::string::npos) {
          response =
              "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: "
              "19\r\nConnection: close\r\n\r\nInternal Server Err";
        } else if (request_header.find("POST /redirect/") != std::string::npos) {
          response = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + std::to_string(port_) +
                     "/v1/chat/completions\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else if (request_header.find("POST /malformed/") != std::string::npos) {
          const std::string body = "{not-json}";
          response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        } else if (request_header.find("POST /empty_cands/") != std::string::npos) {
          const std::string body = R"({"choices":[{"message":{"content":"{\"candidates\":[]}"}}]})";
          response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        } else if (request_header.find("POST /slow/") != std::string::npos) {
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          const std::string body =
              R"({"choices":[{"message":{"content":"{\"candidates\":[\"Slow Result\"]}"}}]})";
          response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        } else {
          // Standard successful OpenAI compatible response
          const std::string body =
              R"({"choices":[{"message":{"content":"{\"candidates\":[\"Candidate 1\",\"Candidate 2\"]}"}}]})";
          response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        }

        send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
        break;
      }
      close(client_fd);
    }
  }

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_requested_{false};
  std::atomic<int> request_count_{0};
  mutable std::mutex mutex_;
  std::vector<std::string> captured_headers_;
  std::vector<std::string> captured_bodies_;
  std::thread worker_thread_;
};

CoreConfig CreateTestConfig(uint16_t port, const std::string& path_prefix = "/v1") {
  CoreConfig config;
  LlmProvider provider;
  provider.id = "test_provider";
  provider.base_url = "http://127.0.0.1:" + std::to_string(port) + path_prefix;
  provider.api_key = "test-secret-key";
  provider.extra_body = {{"custom_knob", 42}};
  config.llm.providers.push_back(provider);
  return config;
}

vinput::scene::Definition CreateTestScene() {
  vinput::scene::Definition scene;
  scene.id = "test_scene";
  scene.provider_id = "test_provider";
  scene.model = "gpt-test-model";
  scene.prompt = "You are a test proofreader.";
  scene.raw_cand = true;
  scene.llm_max_candidates = 2;
  scene.timeout_ms = 5000;
  return scene;
}

void TestProcessSuccess() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/v1");
  const auto scene = CreateTestScene();
  const PostProcessor processor;

  std::string error_out;
  const auto payload = processor.Process("hello world", scene, config, &error_out, nullptr);

  AssertTrue(error_out.empty(), "error_out should be empty on success");
  AssertTrue(payload.commitText == "Candidate 1", "commitText should be first LLM candidate");
  AssertTrue(payload.candidates.size() == 3, "candidates should have raw + 2 LLM candidates");
  AssertTrue(payload.candidates[0].text == "hello world", "first candidate is raw");
  AssertTrue(payload.candidates[0].source == vinput::result::kSourceRaw, "source is raw");
  AssertTrue(payload.candidates[1].text == "Candidate 1", "second candidate is LLM 1");
  AssertTrue(payload.candidates[1].source == vinput::result::kSourceLlm, "source is LLM");
  AssertTrue(payload.candidates[2].text == "Candidate 2", "third candidate is LLM 2");
  AssertTrue(payload.candidates[2].source == vinput::result::kSourceLlm, "source is LLM");

  const auto headers = server.CapturedHeaders();
  AssertTrue(!headers.empty(), "server should receive request header");
  AssertTrue(headers[0].find("Authorization: Bearer test-secret-key") != std::string::npos,
             "Authorization header passed");
  AssertTrue(headers[0].find("Content-Type: application/json") != std::string::npos,
             "Content-Type header passed");

  const auto bodies = server.CapturedBodies();
  AssertTrue(!bodies.empty(), "server should receive request body");
  const auto req_json = nlohmann::json::parse(bodies[0]);
  AssertTrue(req_json["model"] == "gpt-test-model", "model passed in request json");
  AssertTrue(req_json["custom_knob"] == 42, "extra_body passed in request json");

  const auto prompt_content = req_json["messages"][0]["content"].get<std::string>();
  AssertTrue(prompt_content.find("<vinput-asr>\nhello world\n</vinput-asr>") != std::string::npos,
             "ASR text wrapped in XML tags in prompt");
}

void TestProcessCommandSuccess() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/v1");
  vinput::scene::Definition command_scene;
  command_scene.id = "translate";
  command_scene.provider_id = "test_provider";
  command_scene.model = "gpt-test-model";
  command_scene.prompt = "Translate {{asr}} from {{selected}}";
  command_scene.llm_max_candidates = 2;
  command_scene.timeout_ms = 5000;

  const PostProcessor processor;
  std::string error_out;
  const auto payload =
      processor.ProcessCommand("to English", "你好", command_scene, config, &error_out, nullptr);

  AssertTrue(error_out.empty(), "error_out should be empty on command success");
  AssertTrue(payload.commitText == "Candidate 1", "commitText is first LLM result");
  AssertTrue(payload.candidates.size() == 4, "selected + asr + 2 LLM candidates");
  AssertTrue(payload.candidates[0].text == "你好", "first candidate is selected text");
  AssertTrue(payload.candidates[0].source == vinput::result::kSourceRaw, "source is raw");
  AssertTrue(payload.candidates[1].text == "to English", "second candidate is ASR command");
  AssertTrue(payload.candidates[1].source == vinput::result::kSourceAsr, "source is asr");
  AssertTrue(payload.candidates[2].text == "Candidate 1", "third candidate is LLM 1");
  AssertTrue(payload.candidates[3].text == "Candidate 2", "fourth candidate is LLM 2");

  const auto bodies = server.CapturedBodies();
  AssertTrue(!bodies.empty(), "server should receive command request body");
  const auto req_json = nlohmann::json::parse(bodies[0]);
  const auto prompt_content = req_json["messages"][0]["content"].get<std::string>();
  AssertTrue(prompt_content.find("Translate to English from 你好") != std::string::npos,
             "template interpolation applied for command prompt");
}

void TestProcessEmptyInputFallback() {
  const PostProcessor processor;
  const CoreConfig config;
  const auto scene = CreateTestScene();

  const auto payload = processor.Process("   ", scene, config, nullptr, nullptr);
  AssertTrue(payload.candidates.empty(), "empty raw text returns empty payload");

  const auto cmd_payload =
      processor.ProcessCommand("", "selected", scene, config, nullptr, nullptr);
  AssertTrue(cmd_payload.commitText == "selected", "empty ASR falls back to selected text");
}

void TestProcessCancelFlagPreCheck() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/v1");
  const auto scene = CreateTestScene();
  const PostProcessor processor;

  const std::atomic<bool> cancel_flag{true};
  std::string error_out = "previous error";
  const auto payload = processor.Process("test text", scene, config, &error_out, &cancel_flag);

  AssertTrue(server.RequestCount() == 0, "no network request when cancel_flag is already true");
  AssertTrue(error_out.empty(), "error_out should be cleared on cancellation");
  AssertTrue(payload.commitText == "test text", "fallback to raw text");
  AssertTrue(payload.candidates.size() == 1, "only raw fallback candidate");
}

void TestProcessInFlightCancellation() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/slow");
  auto scene = CreateTestScene();
  scene.timeout_ms = 5000;
  const PostProcessor processor;

  std::atomic<bool> cancel_flag{false};
  std::thread cancel_thread([&cancel_flag] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cancel_flag.store(true);
  });

  std::string error_out;
  const auto payload =
      processor.Process("in-flight cancel", scene, config, &error_out, &cancel_flag);
  cancel_thread.join();

  AssertTrue(error_out.empty(), "error_out cleared on in-flight cancellation");
  AssertTrue(payload.commitText == "in-flight cancel", "fallback to raw text on cancel");
}

void TestPostProcessorShutdown() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/v1");
  const auto scene = CreateTestScene();
  PostProcessor processor;
  processor.Shutdown();

  std::string error_out;
  const auto payload = processor.Process("shutdown test", scene, config, &error_out, nullptr);

  AssertTrue(server.RequestCount() == 0, "no network request when processor is shut down");
  AssertTrue(error_out.empty(), "error_out cleared on shutdown");
  AssertTrue(payload.commitText == "shutdown test", "fallback on shutdown");
}

void TestHttp500ErrorFallback() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/error500");
  const auto scene = CreateTestScene();
  const PostProcessor processor;

  std::string error_out;
  const auto payload = processor.Process("500 test", scene, config, &error_out, nullptr);

  AssertTrue(!error_out.empty(), "error_out populated on HTTP 500");
  AssertTrue(error_out.find("500") != std::string::npos, "error_out mentions HTTP 500");
  AssertTrue(payload.commitText == "500 test", "fallback to raw text on error");
}

void TestRedirectErrorFallback() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/redirect");
  const auto scene = CreateTestScene();
  const PostProcessor processor;

  std::string error_out;
  const auto payload = processor.Process("redirect test", scene, config, &error_out, nullptr);

  AssertTrue(!error_out.empty(), "error_out populated on HTTP 302 redirect");
  AssertTrue(error_out.find("302") != std::string::npos, "error_out mentions HTTP 302");
  AssertTrue(payload.commitText == "redirect test", "fallback to raw text on redirect");
}

void TestMalformedJsonFallback() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/malformed");
  const auto scene = CreateTestScene();
  const PostProcessor processor;

  std::string error_out;
  const auto payload = processor.Process("malformed test", scene, config, &error_out, nullptr);
  AssertTrue(payload.commitText == "malformed test", "fallback on malformed json");
}

void TestEmptyCandidatesFallback() {
  const MockHttpServer server;
  const CoreConfig config = CreateTestConfig(server.Port(), "/empty_cands");
  const auto scene = CreateTestScene();
  const PostProcessor processor;

  std::string error_out;
  const auto payload = processor.Process("empty cands test", scene, config, &error_out, nullptr);
  AssertTrue(payload.commitText == "empty cands test", "fallback when candidates array is empty");
}

void TestProviderNotFoundFallback() {
  const CoreConfig config;
  auto scene = CreateTestScene();
  scene.provider_id = "missing_provider";
  const PostProcessor processor;

  const auto payload = processor.Process("missing provider", scene, config, nullptr, nullptr);
  AssertTrue(payload.commitText == "missing provider", "fallback when provider is not found");
}

} // namespace

int main() {
  std::cout << "Running PostProcessor unit tests...\n";
  TestProcessSuccess();
  TestProcessCommandSuccess();
  TestProcessEmptyInputFallback();
  TestProcessCancelFlagPreCheck();
  TestProcessInFlightCancellation();
  TestPostProcessorShutdown();
  TestHttp500ErrorFallback();
  TestRedirectErrorFallback();
  TestMalformedJsonFallback();
  TestEmptyCandidatesFallback();
  TestProviderNotFoundFallback();
  std::cout << "All PostProcessor unit tests passed!\n";
  return 0;
}
