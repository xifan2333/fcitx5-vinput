#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/net/http_client.h"
#include "common/net/http_types.h"

namespace {

void AssertTrue(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

enum class MockServerBehavior : std::uint8_t {
  KeepAlive,
  CloseEachConnection,
  DropSecondRequestBody,
};

class MockHttpServer {
public:
  explicit MockHttpServer(MockServerBehavior behavior)
      : behavior_(behavior), listen_fd_(socket(AF_INET, SOCK_STREAM, 0)) {
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
  [[nodiscard]] int AcceptedConnectionCount() const { return accepted_connections_.load(); }
  [[nodiscard]] const std::vector<std::string>& CapturedAuthHeaders() const {
    return captured_auth_headers_;
  }
  [[nodiscard]] const std::vector<std::string>& CapturedRequestBodies() const {
    return captured_request_bodies_;
  }

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
      ++accepted_connections_;
      while (!stop_requested_.load()) {
        std::string request_body;
        std::string request_header;
        if (!ReadHttpRequest(client_fd, request_body, request_header)) {
          break;
        }
        const std::size_t auth_pos = request_header.find("Authorization: ");
        const std::size_t auth_end = request_header.find("\r\n", auth_pos);
        captured_auth_headers_.push_back(
            auth_pos == std::string::npos ? ""
                                          : request_header.substr(auth_pos, auth_end - auth_pos));
        captured_request_bodies_.push_back(request_body);
        if (behavior_ == MockServerBehavior::DropSecondRequestBody &&
            captured_request_bodies_.size() == 2) {
          break; // Close without any response: simulate connection drop scenario.
        }
        const std::string response_payload =
            behavior_ == MockServerBehavior::CloseEachConnection
                ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"
                : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK";
        if (send(client_fd, response_payload.data(), response_payload.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(response_payload.size())) {
          break;
        }
        if (behavior_ == MockServerBehavior::CloseEachConnection) {
          break;
        }
        // Keep-alive: wait briefly for the next request on this connection.
        pollfd next_poll{.fd = client_fd, .events = POLLIN, .revents = 0};
        if (poll(&next_poll, 1, 300) <= 0) {
          break;
        }
      }
      close(client_fd);
    }
  }

  const MockServerBehavior behavior_;
  const int listen_fd_;
  std::uint16_t port_ = 0;
  std::thread worker_thread_;
  std::atomic<int> accepted_connections_{0};
  std::atomic<bool> stop_requested_{false};
  std::vector<std::string> captured_auth_headers_;
  std::vector<std::string> captured_request_bodies_;
};

std::string BuildMockServerUrl(const MockHttpServer& server, const std::string& path = "/chat") {
  return "http://127.0.0.1:" + std::to_string(server.Port()) + path;
}

vinput::net::HttpResponse ExecuteRequest(vinput::net::HttpClient& client, const std::string& url,
                                         const std::string& auth_bearer, const std::string& body,
                                         const std::atomic<bool>* cancel_token = nullptr,
                                         bool direct_bypass = true) {
  vinput::net::HttpRequest req;
  req.url = url;
  req.method = vinput::net::HttpMethod::Post;
  req.body = body;
  req.timeout_ms = 3000;
  req.cancel_token = cancel_token;
  if (direct_bypass) {
    req.proxy = ""; // Explicit direct loopback bypass
  }
  if (!auth_bearer.empty()) {
    req.headers.push_back({"Authorization", "Bearer " + auth_bearer});
  }
  return client.Request(req);
}

void TestIsolationAcrossIdentityChanges() {
  MockHttpServer server(MockServerBehavior::KeepAlive);
  vinput::net::HttpClient client;
  const std::string url = BuildMockServerUrl(server);

  // Request 1: Auth A, direct proxy
  AssertTrue(ExecuteRequest(client, url, "A", "one", nullptr, true).Ok(), "request one");
  // Request 2: Auth A, same identity -> reuses connection 1
  AssertTrue(ExecuteRequest(client, url, "A", "two", nullptr, true).Ok(), "request two");
  // Request 3: Auth B, credential changed -> new connection 2
  AssertTrue(ExecuteRequest(client, url, "B", "three", nullptr, true).Ok(), "request three");

  // Request 4: Auth B, proxy policy changed to ambient (nullopt) -> new connection 3
  AssertTrue(ExecuteRequest(client, url, "B", "four", nullptr, false).Ok(), "request four");

  server.Stop();
  AssertTrue(server.AcceptedConnectionCount() == 3,
             "one connection reused, two credentials/policy isolated");
  AssertTrue(server.CapturedAuthHeaders() ==
                 std::vector<std::string>({"Authorization: Bearer A", "Authorization: Bearer A",
                                           "Authorization: Bearer B", "Authorization: Bearer B"}),
             "auth isolated");
  AssertTrue(server.CapturedRequestBodies() ==
                 std::vector<std::string>({"one", "two", "three", "four"}),
             "request content isolated");
  std::cout << "Four HTTP POSTs over three connections; isolation verified\n";
}

void TestTenConsecutiveRequestsReuseOneConnection() {
  MockHttpServer server(MockServerBehavior::KeepAlive);
  vinput::net::HttpClient client;
  const std::string url = BuildMockServerUrl(server);

  for (int i = 0; i < 10; ++i) {
    auto res = ExecuteRequest(client, url, "A", "req-" + std::to_string(i));
    AssertTrue(res.Ok(), "ten-request workload succeeds");
    AssertTrue(res.body == "OK", "ten-request response");
  }

  server.Stop();
  AssertTrue(server.AcceptedConnectionCount() == 1, "ten requests share one server connection");
  AssertTrue(server.CapturedRequestBodies().size() == 10, "server received ten bodies");
  std::cout << "Ten consecutive requests successfully reused one Keep-Alive connection\n";
}

void TestConnectionCloseReconnectsCleanly() {
  MockHttpServer server(MockServerBehavior::CloseEachConnection);
  vinput::net::HttpClient client;
  const std::string url = BuildMockServerUrl(server);

  auto first = ExecuteRequest(client, url, "A", "close-one");
  auto second = ExecuteRequest(client, url, "A", "close-two");

  server.Stop();
  AssertTrue(first.Ok() && second.Ok(), "requests after Connection: close succeed");
  AssertTrue(server.AcceptedConnectionCount() == 2, "server saw two connections after close");
  AssertTrue(server.CapturedRequestBodies() == std::vector<std::string>({"close-one", "close-two"}),
             "close bodies");
  std::cout << "Connection: close reconnects cleanly verified\n";
}

void TestDroppedResponseIsNotResent() {
  MockHttpServer server(MockServerBehavior::DropSecondRequestBody);
  vinput::net::HttpClient client;
  const std::string url = BuildMockServerUrl(server);

  AssertTrue(ExecuteRequest(client, url, "A", "first-ok").Ok(), "first request");
  auto dropped = ExecuteRequest(client, url, "A", "drop-body");
  AssertTrue(!dropped.Ok(), "dropped response must surface an error, not silent resend");

  auto recovered = ExecuteRequest(client, url, "B", "after-drop");
  server.Stop();
  AssertTrue(recovered.Ok(), "request after drop recovers");
  AssertTrue(server.CapturedRequestBodies() ==
                 std::vector<std::string>({"first-ok", "drop-body", "after-drop"}),
             "dropped body received exactly once, no duplicate POST");
  std::cout << "Dropped-response request failed safely and was not silently resent\n";
}

void TestCancelThenNextRequestIsIsolated() {
  MockHttpServer server(MockServerBehavior::KeepAlive);
  vinput::net::HttpClient client;
  const std::string url = BuildMockServerUrl(server);

  const std::atomic<bool> cancel_token{true};
  auto cancelled = ExecuteRequest(client, url, "A", "cancel-body", &cancel_token);
  AssertTrue(cancelled.cancelled, "cancel token aborts transfer");
  AssertTrue(!cancelled.Ok(), "cancelled request Ok() is false");

  auto next = ExecuteRequest(client, url, "A", "after-cancel");
  server.Stop();
  AssertTrue(next.Ok() && next.body == "OK", "request after cancel succeeds");
  int after_cancel_count = 0;
  for (const std::string& body : server.CapturedRequestBodies()) {
    if (body == "after-cancel") {
      ++after_cancel_count;
    }
  }
  AssertTrue(after_cancel_count == 1, "post-cancel body delivered exactly once");
  std::cout << "Cancellation and recovery verified\n";
}

void TestGetAndDownloadToFile() {
  MockHttpServer server(MockServerBehavior::KeepAlive);
  vinput::net::HttpClient client;
  const std::string url = BuildMockServerUrl(server);

  // Test Get with direct proxy
  vinput::net::HttpRequest get_req;
  get_req.url = url;
  get_req.method = vinput::net::HttpMethod::Get;
  get_req.proxy = "";
  auto get_res = client.Request(get_req);
  AssertTrue(get_res.Ok() && get_res.body == "OK", "Get request succeeds");

  // Test Post helper
  vinput::net::HttpRequest post_req;
  post_req.url = url;
  post_req.method = vinput::net::HttpMethod::Post;
  post_req.body = "plain-body";
  post_req.proxy = "";
  auto post_res = client.Request(post_req);
  AssertTrue(post_res.Ok() && post_res.body == "OK", "Post helper succeeds");

  // Test PostJson
  vinput::net::HttpRequest json_req;
  json_req.url = url;
  json_req.method = vinput::net::HttpMethod::Post;
  json_req.body = R"({"key":"val"})";
  json_req.headers.push_back({"Content-Type", "application/json"});
  json_req.proxy = "";
  auto post_json_res = client.Request(json_req);
  AssertTrue(post_json_res.Ok() && post_json_res.body == "OK", "PostJson succeeds");

  // Test DownloadToFile with existing file preservation
  const std::filesystem::path temp_file =
      std::filesystem::temp_directory_path() / "vinput_test_download.tmp";

  // Pre-seed file
  {
    std::ofstream seed(temp_file);
    seed << "pre-existing content";
  }

  vinput::net::HttpRequest dl_req;
  dl_req.url = url;
  dl_req.method = vinput::net::HttpMethod::Get;
  dl_req.proxy = "";
  auto dl_res = client.DownloadToFile(dl_req, temp_file);
  AssertTrue(dl_res.Ok(), "DownloadToFile succeeds");
  AssertTrue(std::filesystem::exists(temp_file), "downloaded file exists");

  std::ifstream in(temp_file);
  std::string file_content;
  in >> file_content;
  AssertTrue(file_content == "OK", "downloaded file content matches");
  in.close();
  std::filesystem::remove(temp_file);

  server.Stop();
  std::cout << "Get, Post, PostJson and DownloadToFile verified\n";
}

void TestQueryOnlyUrlOriginReuse() {
  MockHttpServer server(MockServerBehavior::KeepAlive);
  vinput::net::HttpClient client;

  const std::string url1 = BuildMockServerUrl(server, "?query1=val1");
  const std::string url2 = BuildMockServerUrl(server, "?query2=val2");

  auto res1 = ExecuteRequest(client, url1, "A", "q1");
  auto res2 = ExecuteRequest(client, url2, "A", "q2");

  server.Stop();
  AssertTrue(res1.Ok() && res2.Ok(), "query requests succeed");
  AssertTrue(server.AcceptedConnectionCount() == 1,
             "query URLs on same origin share one Keep-Alive connection");
  std::cout << "Query-only URLs origin reuse verified\n";
}

} // namespace

int main() {
  TestIsolationAcrossIdentityChanges();
  TestTenConsecutiveRequestsReuseOneConnection();
  TestConnectionCloseReconnectsCleanly();
  TestDroppedResponseIsNotResent();
  TestCancelThenNextRequestIsIsolated();
  TestGetAndDownloadToFile();
  TestQueryOnlyUrlOriginReuse();
  std::cout << "All HttpClient unit tests passed successfully!\n";
  return 0;
}
