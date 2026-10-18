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
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/utils/downloader.h"

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
  [[nodiscard]] int RequestCount() const { return request_count_.load(); }

private:
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

      std::string buffer;
      while (!stop_requested_.load()) {
        pollfd client_pfd{.fd = client_fd, .events = POLLIN, .revents = 0};
        if (poll(&client_pfd, 1, 100) <= 0) {
          continue;
        }
        char chunk[2048];
        const ssize_t bytes_read = recv(client_fd, chunk, sizeof(chunk), 0);
        if (bytes_read <= 0) {
          break;
        }
        buffer.append(chunk, static_cast<size_t>(bytes_read));
        const auto head_end = buffer.find("\r\n\r\n");
        if (head_end != std::string::npos) {
          ++request_count_;
          if (buffer.find("GET /404 ") != std::string::npos) {
            const std::string response =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
          } else if (buffer.find("GET /204 ") != std::string::npos) {
            const std::string response =
                "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
          } else if (buffer.find("GET /redirect ") != std::string::npos) {
            const std::string response =
                "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + std::to_string(port_) +
                "/text\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
          } else if (buffer.find("GET /slow ") != std::string::npos) {
            const std::string header =
                "HTTP/1.1 200 OK\r\nContent-Length: 20000\r\nConnection: close\r\n\r\n";
            send(client_fd, header.data(), header.size(), MSG_NOSIGNAL);
            const std::string chunk(50, 'Z');
            for (int i = 0; i < 20 && !stop_requested_.load(); ++i) {
              if (send(client_fd, chunk.data(), chunk.size(), MSG_NOSIGNAL) <= 0) {
                break;
              }
              usleep(15000);
            }
          } else if (buffer.find("GET /large ") != std::string::npos) {
            const std::string body(100, 'X');
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
          } else {
            const std::string body = "Hello Downloader";
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
          }
          break;
        }
      }
      close(client_fd);
    }
  }

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_requested_{false};
  std::atomic<int> request_count_{0};
  std::thread worker_thread_;
};

std::string BuildUrl(const MockHttpServer& server, const std::string& path) {
  return "http://127.0.0.1:" + std::to_string(server.Port()) + path;
}

vinput::download::Options MakeTestOptions() {
  vinput::download::Options opts;
  opts.proxy = ""; // Direct loopback bypass without touching environment
  return opts;
}

void TestDownloadTextSuccess() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/text");

  const auto opts = MakeTestOptions();
  vinput::download::Result result;
  std::string content;
  const bool ok = vinput::download::DownloadText({url}, opts, &content, &result);

  server.Stop();
  AssertTrue(ok, "DownloadText succeeds");
  AssertTrue(content == "Hello Downloader", "content matches");
  AssertTrue(result.ok, "result.ok is true");
  AssertTrue(result.http_code == 200, "result.http_code is 200");
  AssertTrue(result.bytes_received == content.size(), "bytes_received matches");
  AssertTrue(result.resolved_url == url, "resolved_url matches");
  std::cout << "TestDownloadTextSuccess passed\n";
}

void TestDownloadTextFallback() {
  MockHttpServer server;
  const std::string url404 = BuildUrl(server, "/404");
  const std::string url_ok = BuildUrl(server, "/text");

  const auto opts = MakeTestOptions();
  vinput::download::Result result;
  std::string content;
  const bool ok = vinput::download::DownloadText({url404, url_ok}, opts, &content, &result);

  server.Stop();
  AssertTrue(ok, "DownloadText fallback succeeds");
  AssertTrue(content == "Hello Downloader", "content matches");
  AssertTrue(result.ok, "result.ok is true");
  AssertTrue(result.resolved_url == url_ok, "resolved_url matches second URL");
  AssertTrue(server.RequestCount() == 2, "server received 2 requests");
  std::cout << "TestDownloadTextFallback passed\n";
}

void TestDownloadTextMaxBytes() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/large");

  auto opts = MakeTestOptions();
  opts.max_bytes = 20; // Server returns 100 bytes
  vinput::download::Result result;
  std::string content;
  const bool ok = vinput::download::DownloadText({url}, opts, &content, &result);

  server.Stop();
  AssertTrue(!ok, "DownloadText fails when exceeding max_bytes");
  AssertTrue(!result.ok, "result.ok is false");
  std::cout << "TestDownloadTextMaxBytes passed\n";
}

void TestDownloadTextCancellation() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/text");
  const std::string url2 = BuildUrl(server, "/text2");

  const std::atomic<bool> cancel_token{true};
  auto opts = MakeTestOptions();
  opts.cancel_token = &cancel_token;
  vinput::download::Result result;
  std::string content;
  const bool ok = vinput::download::DownloadText({url, url2}, opts, &content, &result);

  server.Stop();
  AssertTrue(!ok, "DownloadText fails when cancelled");
  AssertTrue(!result.ok, "result.ok is false");
  AssertTrue(result.error == "download cancelled", "error is download cancelled");
  AssertTrue(server.RequestCount() == 0, "server saw no requests on pre-cancelled token");
  std::cout << "TestDownloadTextCancellation passed\n";
}

void TestDownloadTextActiveCancellation() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/slow");

  std::atomic<bool> cancel_token{false};
  auto opts = MakeTestOptions();
  opts.cancel_token = &cancel_token;
  opts.progress_cb = [&](const vinput::download::Progress& prog) {
    if (prog.downloaded_bytes > 0) {
      cancel_token.store(true, std::memory_order_relaxed);
    }
  };

  vinput::download::Result result;
  std::string content;
  const bool ok = vinput::download::DownloadText({url}, opts, &content, &result);

  server.Stop();
  AssertTrue(!ok, "DownloadText fails on in-flight cancellation");
  AssertTrue(!result.ok, "result.ok is false");
  AssertTrue(result.error == "download cancelled", "error is download cancelled");
  std::cout << "TestDownloadTextActiveCancellation passed\n";
}

void TestDownloadHttp204Fallback() {
  MockHttpServer server;
  const std::string url204 = BuildUrl(server, "/204");
  const std::string url_ok = BuildUrl(server, "/text");

  const auto opts = MakeTestOptions();
  vinput::download::Result result;
  std::string content;
  const bool ok = vinput::download::DownloadText({url204, url_ok}, opts, &content, &result);

  server.Stop();
  AssertTrue(ok, "DownloadText fallback from 204 succeeds");
  AssertTrue(content == "Hello Downloader", "content matches");
  AssertTrue(result.ok, "result.ok is true");
  AssertTrue(result.resolved_url == url_ok, "resolved_url matches second URL");
  AssertTrue(server.RequestCount() == 2, "server received 2 requests");
  std::cout << "TestDownloadHttp204Fallback passed\n";
}

void TestDownloadRedirectFollowed() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/redirect");
  const std::string expected_target = BuildUrl(server, "/text");

  const auto opts = MakeTestOptions();
  vinput::download::Result result;
  std::string content;
  const bool ok = vinput::download::DownloadText({url}, opts, &content, &result);

  server.Stop();
  AssertTrue(ok, "DownloadText with redirect succeeds");
  AssertTrue(content == "Hello Downloader", "content matches");
  AssertTrue(result.ok, "result.ok is true");
  AssertTrue(result.resolved_url == expected_target,
             "resolved_url reflects the effective redirected target");
  std::cout << "TestDownloadRedirectFollowed passed\n";
}

void TestDownloadFileSuccess() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/text");
  const auto dest = std::filesystem::temp_directory_path() /
                    ("vinput_test_dl_" + std::to_string(getpid()) + ".txt");

  std::error_code ec;
  std::filesystem::remove(dest, ec);

  const auto opts = MakeTestOptions();
  vinput::download::Result result;
  const bool ok = vinput::download::DownloadFile({url}, dest, opts, &result);

  server.Stop();
  AssertTrue(ok, "DownloadFile succeeds");
  AssertTrue(result.ok, "result.ok is true");
  AssertTrue(std::filesystem::exists(dest), "dest file exists");

  std::ifstream in(dest);
  std::string file_content;
  std::getline(in, file_content);
  AssertTrue(file_content == "Hello Downloader", "file content matches");
  in.close();

  std::filesystem::remove(dest, ec);
  std::cout << "TestDownloadFileSuccess passed\n";
}

void TestDownloadFileFallback() {
  MockHttpServer server;
  const std::string url404 = BuildUrl(server, "/404");
  const std::string url_ok = BuildUrl(server, "/text");
  const auto dest = std::filesystem::temp_directory_path() /
                    ("vinput_test_dl_fb_" + std::to_string(getpid()) + ".txt");

  std::error_code ec;
  std::filesystem::remove(dest, ec);

  const auto opts = MakeTestOptions();
  vinput::download::Result result;
  const bool ok = vinput::download::DownloadFile({url404, url_ok}, dest, opts, &result);

  server.Stop();
  AssertTrue(ok, "DownloadFile fallback succeeds");
  AssertTrue(result.ok, "result.ok is true");
  AssertTrue(result.resolved_url == url_ok, "resolved_url matches second URL");
  AssertTrue(std::filesystem::exists(dest), "dest file exists");

  std::filesystem::remove(dest, ec);
  std::cout << "TestDownloadFileFallback passed\n";
}

void TestDownloadFileCancellation() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/text");
  const auto dest = std::filesystem::temp_directory_path() /
                    ("vinput_test_dl_cancel_" + std::to_string(getpid()) + ".txt");

  std::error_code ec;
  std::filesystem::remove(dest, ec);

  const std::atomic<bool> cancel_token{true};
  auto opts = MakeTestOptions();
  opts.cancel_token = &cancel_token;
  vinput::download::Result result;
  const bool ok = vinput::download::DownloadFile({url}, dest, opts, &result);

  server.Stop();
  AssertTrue(!ok, "DownloadFile fails when cancelled");
  AssertTrue(!result.ok, "result.ok is false");
  AssertTrue(result.error == "download cancelled", "error is download cancelled");
  AssertTrue(!std::filesystem::exists(dest), "dest file was not created");
  std::cout << "TestDownloadFileCancellation passed\n";
}

void TestDownloadFileActiveCancellation() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/slow");
  const auto dest = std::filesystem::temp_directory_path() /
                    ("vinput_test_dl_actcancel_" + std::to_string(getpid()) + ".txt");

  std::error_code ec;
  std::filesystem::remove(dest, ec);

  std::atomic<bool> cancel_token{false};
  auto opts = MakeTestOptions();
  opts.cancel_token = &cancel_token;
  opts.progress_cb = [&](const vinput::download::Progress& prog) {
    if (prog.downloaded_bytes > 0) {
      cancel_token.store(true, std::memory_order_relaxed);
    }
  };

  vinput::download::Result result;
  const bool ok = vinput::download::DownloadFile({url}, dest, opts, &result);

  server.Stop();
  AssertTrue(!ok, "DownloadFile fails on in-flight cancellation");
  AssertTrue(!result.ok, "result.ok is false");
  AssertTrue(result.error == "download cancelled", "error is download cancelled");
  AssertTrue(!std::filesystem::exists(dest), "dest file was removed on cancellation");
  std::cout << "TestDownloadFileActiveCancellation passed\n";
}

void TestDownloadFileProgressCallback() {
  MockHttpServer server;
  const std::string url = BuildUrl(server, "/text");
  const auto dest = std::filesystem::temp_directory_path() /
                    ("vinput_test_dl_prog_" + std::to_string(getpid()) + ".txt");

  std::error_code ec;
  std::filesystem::remove(dest, ec);

  bool progress_called = false;
  auto opts = MakeTestOptions();
  opts.progress_cb = [&](const vinput::download::Progress& prog) {
    if (prog.downloaded_bytes > 0) {
      progress_called = true;
    }
  };
  vinput::download::Result result;
  const bool ok = vinput::download::DownloadFile({url}, dest, opts, &result);

  server.Stop();
  AssertTrue(ok, "DownloadFile with progress succeeds");
  AssertTrue(progress_called, "progress callback was invoked");

  std::filesystem::remove(dest, ec);
  std::cout << "TestDownloadFileProgressCallback passed\n";
}

void TestDownloadFileFailureCleansUpExistingDest() {
  MockHttpServer server;
  const std::string url404 = BuildUrl(server, "/404");
  const auto dest = std::filesystem::temp_directory_path() /
                    ("vinput_test_dl_cleanup_" + std::to_string(getpid()) + ".txt");

  {
    std::ofstream out(dest);
    out << "stale data";
  }
  AssertTrue(std::filesystem::exists(dest), "stale file exists before test");

  const auto opts = MakeTestOptions();
  vinput::download::Result result;
  const bool ok = vinput::download::DownloadFile({url404}, dest, opts, &result);

  server.Stop();
  AssertTrue(!ok, "DownloadFile fails for 404");
  AssertTrue(!result.ok, "result.ok is false");
  AssertTrue(!std::filesystem::exists(dest), "dest file was removed on failure");
  std::cout << "TestDownloadFileFailureCleansUpExistingDest passed\n";
}

void TestDownloadLocalFileUrl() {
  const auto src_file = std::filesystem::temp_directory_path() /
                        ("vinput_test_src_" + std::to_string(getpid()) + ".txt");
  const auto dest_file = std::filesystem::temp_directory_path() /
                         ("vinput_test_dst_" + std::to_string(getpid()) + ".txt");

  std::error_code ec;
  std::filesystem::remove(src_file, ec);
  std::filesystem::remove(dest_file, ec);

  {
    std::ofstream out(src_file);
    out << "Local File Content";
  }

  const std::string file_url = "file://" + src_file.string();
  const auto opts = MakeTestOptions();

  std::string content;
  vinput::download::Result text_res;
  const bool text_ok = vinput::download::DownloadText({file_url}, opts, &content, &text_res);
  AssertTrue(text_ok, "DownloadText from file:// succeeds");
  AssertTrue(content == "Local File Content", "text content matches local file");

  vinput::download::Result file_res;
  const bool file_ok = vinput::download::DownloadFile({file_url}, dest_file, opts, &file_res);
  AssertTrue(file_ok, "DownloadFile from file:// succeeds");
  AssertTrue(std::filesystem::exists(dest_file), "dest file created from file://");

  std::filesystem::remove(src_file, ec);
  std::filesystem::remove(dest_file, ec);
  std::cout << "TestDownloadLocalFileUrl passed\n";
}

} // namespace

int main() {
  TestDownloadTextSuccess();
  TestDownloadTextFallback();
  TestDownloadTextMaxBytes();
  TestDownloadTextCancellation();
  TestDownloadTextActiveCancellation();
  TestDownloadHttp204Fallback();
  TestDownloadRedirectFollowed();
  TestDownloadFileSuccess();
  TestDownloadFileFallback();
  TestDownloadFileCancellation();
  TestDownloadFileActiveCancellation();
  TestDownloadFileProgressCallback();
  TestDownloadFileFailureCleansUpExistingDest();
  TestDownloadLocalFileUrl();
  std::cout << "All Downloader unit tests passed successfully!\n";
  return 0;
}
