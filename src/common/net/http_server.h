#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/net/http_types.h"

namespace vinput::net {

class WebSocketSession {
public:
  virtual ~WebSocketSession() = default;
  WebSocketSession() = default;
  WebSocketSession(const WebSocketSession&) = delete;
  WebSocketSession& operator=(const WebSocketSession&) = delete;
  WebSocketSession(WebSocketSession&&) = delete;
  WebSocketSession& operator=(WebSocketSession&&) = delete;

  virtual bool SendText(std::string_view text) = 0;
  virtual bool SendBinary(std::string_view binary) = 0;
  virtual bool SendJson(std::string_view json_text) = 0;
  virtual bool SendPing(std::string_view payload = {}) = 0;
  virtual bool SendPong(std::string_view payload = {}) = 0;
  virtual void Close(std::uint16_t status_code = 1000, std::string_view reason = {}) = 0;
  [[nodiscard]] virtual bool IsOpen() const = 0;

  virtual void
  SetOnMessage(std::function<void(std::string_view payload, bool is_binary)> callback) = 0;
  virtual void
  SetOnClose(std::function<void(std::uint16_t status_code, std::string_view reason)> callback) = 0;
  virtual void SetOnError(std::function<void(std::string_view error)> callback) = 0;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;
using WebSocketHandler = std::function<void(std::shared_ptr<WebSocketSession>, const HttpRequest&)>;
using WebSocketAuthorizer = std::function<std::optional<HttpResponse>(const HttpRequest&)>;

struct HttpServerConfig {
  std::string host = "0.0.0.0";
  std::uint16_t port = 0; // 0 for automatic ephemeral port allocation
  std::size_t max_request_body_bytes = 1024ULL * 1024ULL;
  std::size_t max_ws_payload_bytes = 16ULL * 1024ULL * 1024ULL;
  std::size_t max_connections = 64;
  int read_timeout_seconds = 15;
};

struct WebSocketEndpoint {
  WebSocketHandler handler;
  WebSocketAuthorizer authorizer;
};

class HttpServer {
public:
  explicit HttpServer(HttpServerConfig config = {});
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) = delete;
  HttpServer& operator=(HttpServer&&) = delete;

  // Route registration
  void Get(std::string path, HttpHandler handler);
  void Post(std::string path, HttpHandler handler);
  void Route(HttpMethod method, std::string path, HttpHandler handler);

  // WebSocket upgrade registration
  void UpgradeWebSocket(std::string path, WebSocketHandler handler,
                        WebSocketAuthorizer authorizer = nullptr);
  void UpgradeWebSocket(std::string path,
                        std::function<void(std::shared_ptr<WebSocketSession>)> on_connect,
                        std::function<void(std::string_view, bool)> on_message,
                        std::function<void(std::uint16_t, std::string_view)> on_close = nullptr,
                        std::function<void(std::string_view)> on_error = nullptr);

  // Lifecycle management
  bool Start();
  void Stop();
  [[nodiscard]] bool IsRunning() const;

  [[nodiscard]] std::uint16_t BoundPort() const;
  [[nodiscard]] std::string BoundAddress() const;

private:
  struct ClientWorker {
    std::thread thread;
    std::atomic<int> fd{-1};
    std::atomic<bool> done{false};
  };

  void AcceptLoop();
  void HandleClient(const std::shared_ptr<ClientWorker>& worker);
  void PruneFinishedWorkers();

  HttpServerConfig config_;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  int stop_pipe_[2] = {-1, -1};
  std::uint16_t bound_port_ = 0;
  std::string bound_address_;
  std::thread accept_thread_;

  mutable std::mutex routes_mutex_;
  std::map<std::string, std::map<HttpMethod, HttpHandler>> routes_;
  std::map<std::string, WebSocketEndpoint> ws_endpoints_;

  mutable std::mutex workers_mutex_;
  std::vector<std::shared_ptr<ClientWorker>> workers_;
};

} // namespace vinput::net
