#include "common/net/http_server.h"

#include <asm-generic/socket.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/net/http_types.h"
#include "common/net/websocket_frame.h"
#include "common/utils/string_utils.h"

namespace vinput::net {

namespace {

constexpr std::size_t kMaxHeaderBytes = 64ULL * 1024ULL; // 64 KB header limit

struct HttpStatusEntry {
  long code;
  std::string_view reason;
};

constexpr HttpStatusEntry kHttpStatusTable[] = {
    {200, "OK"},
    {201, "Created"},
    {204, "No Content"},
    {205, "Reset Content"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {304, "Not Modified"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {413, "Payload Too Large"},
    {426, "Upgrade Required"},
    {431, "Request Header Fields Too Large"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {503, "Service Unavailable"},
};

std::string_view HttpStatusReason(long status_code) noexcept {
  for (const auto& entry : kHttpStatusTable) {
    if (entry.code == status_code) {
      return entry.reason;
    }
  }
  return "Unknown";
}

struct HttpMethodEntry {
  HttpMethod method;
  std::string_view name;
};

constexpr HttpMethodEntry kHttpMethodTable[] = {
    {HttpMethod::Get, "GET"},       {HttpMethod::Post, "POST"}, {HttpMethod::Put, "PUT"},
    {HttpMethod::Delete, "DELETE"}, {HttpMethod::Head, "HEAD"},
};

std::string_view MethodToString(HttpMethod method) noexcept {
  for (const auto& entry : kHttpMethodTable) {
    if (entry.method == method) {
      return entry.name;
    }
  }
  return "GET";
}

std::optional<HttpMethod> StringToMethod(std::string_view str) noexcept {
  for (const auto& entry : kHttpMethodTable) {
    if (entry.name == str) {
      return entry.method;
    }
  }
  return std::nullopt;
}

bool WriteAll(int fd, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
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

bool HasHeader(const std::vector<HttpHeader>& headers, std::string_view name) {
  return FindHeader(headers, name).has_value();
}

bool HeaderContainsToken(const std::vector<HttpHeader>& headers, std::string_view name,
                         std::string_view token) {
  const auto val = FindHeader(headers, name);
  if (!val) {
    return false;
  }
  std::string_view remaining = *val;
  while (!remaining.empty()) {
    const auto comma_pos = remaining.find(',');
    const std::string_view current =
        (comma_pos == std::string_view::npos) ? remaining : remaining.substr(0, comma_pos);
    if (vinput::str::EqualsIgnoreCase(vinput::str::TrimAsciiWhitespaceView(current), token)) {
      return true;
    }
    if (comma_pos == std::string_view::npos) {
      break;
    }
    remaining = remaining.substr(comma_pos + 1);
  }
  return false;
}

bool IsWebSocketUpgradeRequest(const std::vector<HttpHeader>& headers) {
  return HeaderContainsToken(headers, "Upgrade", "websocket") &&
         HeaderContainsToken(headers, "Connection", "Upgrade");
}

bool SendHttpResponse(int fd, const HttpResponse& response, HttpMethod method = HttpMethod::Get) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status_code << " " << HttpStatusReason(response.status_code)
      << "\r\n";

  const bool is_bodyless = (response.status_code == 204 || response.status_code == 304 ||
                            response.status_code == 205 || method == HttpMethod::Head);

  if (!is_bodyless && !HasHeader(response.headers, "Content-Length")) {
    out << "Content-Length: " << response.body.size() << "\r\n";
  }
  if (!HasHeader(response.headers, "Connection")) {
    out << "Connection: close\r\n";
  }
  if (!is_bodyless && !HasHeader(response.headers, "Content-Type") && !response.body.empty()) {
    out << "Content-Type: text/plain; charset=utf-8\r\n";
  }
  for (const auto& h : response.headers) {
    out << h.name << ": " << h.value << "\r\n";
  }
  out << "\r\n";

  if (!is_bodyless) {
    out << response.body;
  }
  return WriteAll(fd, out.str());
}

bool SendErrorResponse(int fd, long status_code, std::string_view message,
                       const std::vector<HttpHeader>& extra_headers = {}) {
  HttpResponse resp;
  resp.status_code = status_code;
  resp.body = std::string(message);
  resp.headers = extra_headers;
  return SendHttpResponse(fd, resp);
}

bool ReadHttpHeaders(int fd, std::string* buffer, std::size_t max_bytes, int timeout_ms) {
  char chunk[4096];
  while (buffer->find("\r\n\r\n") == std::string::npos) {
    if (buffer->size() > max_bytes) {
      SendErrorResponse(fd, 431, "Request Header Fields Too Large\n");
      return false;
    }
    if (timeout_ms > 0) {
      pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
      const int pret = poll(&pfd, 1, timeout_ms);
      if (pret <= 0) {
        return false;
      }
    }
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n > 0) {
      buffer->append(chunk, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }

  const std::size_t header_end = buffer->find("\r\n\r\n");
  if (header_end > max_bytes) {
    SendErrorResponse(fd, 431, "Request Header Fields Too Large\n");
    return false;
  }
  return true;
}

bool ParseHttpRequestHeader(std::string_view raw_data, HttpRequest* out_req,
                            std::string* out_initial_body) {
  const std::size_t header_end = raw_data.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return false;
  }

  const std::string headers_part(raw_data.substr(0, header_end));
  *out_initial_body = std::string(raw_data.substr(header_end + 4));

  std::istringstream stream(headers_part);
  std::string request_line;
  if (!std::getline(stream, request_line)) {
    return false;
  }
  if (!request_line.empty() && request_line.back() == '\r') {
    request_line.pop_back();
  }

  std::istringstream req_stream(request_line);
  std::string method_str;
  std::string target;
  std::string http_version;
  req_stream >> method_str >> target >> http_version;

  const auto method = StringToMethod(method_str);
  if (!method || target.empty()) {
    return false;
  }

  std::vector<HttpHeader> headers;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto pair = vinput::str::SplitPair(line, ':', /*trim_whitespace=*/true);
    if (pair) {
      headers.push_back({std::string(pair->first), std::string(pair->second)});
    }
  }

  out_req->url = std::move(target);
  out_req->method = *method;
  out_req->headers = std::move(headers);
  return true;
}

std::optional<std::size_t> ExtractContentLength(const std::vector<HttpHeader>& headers) {
  const auto cl_val = FindHeader(headers, "Content-Length");
  if (!cl_val) {
    return 0;
  }
  const std::string_view trimmed = vinput::str::TrimAsciiWhitespaceView(*cl_val);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  for (const char ch : trimmed) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
  }
  return static_cast<std::size_t>(std::strtoull(std::string(trimmed).c_str(), nullptr, 10));
}

bool ReadHttpRequestBody(int fd, std::string* body, std::size_t expected_length, int timeout_ms) {
  char chunk[4096];
  while (body->size() < expected_length) {
    if (timeout_ms > 0) {
      pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
      const int pret = poll(&pfd, 1, timeout_ms);
      if (pret <= 0) {
        return false;
      }
    }
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n > 0) {
      body->append(chunk, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  body->resize(expected_length);
  return true;
}

constexpr bool IsControlOpcode(WsOpcode opcode) noexcept {
  return static_cast<std::uint8_t>(opcode) >= 0x8;
}

struct WsFragmentState {
  bool in_fragment = false;
  std::string buffer;
  WsOpcode opcode = WsOpcode::kText;
};

class WebSocketSessionImpl : public WebSocketSession,
                             public std::enable_shared_from_this<WebSocketSessionImpl> {
public:
  explicit WebSocketSessionImpl(int fd) : read_fd_(fd), fd_(fd) {}

  ~WebSocketSessionImpl() override { CloseInternal(1000, "", false); }

  WebSocketSessionImpl(const WebSocketSessionImpl&) = delete;
  WebSocketSessionImpl& operator=(const WebSocketSessionImpl&) = delete;
  WebSocketSessionImpl(WebSocketSessionImpl&&) = delete;
  WebSocketSessionImpl& operator=(WebSocketSessionImpl&&) = delete;

  bool SendText(std::string_view text) override { return SendRaw(EncodeWebSocketText(text)); }

  bool SendBinary(std::string_view binary) override {
    return SendRaw(EncodeWebSocketBinary(binary));
  }

  bool SendJson(std::string_view json_text) override { return SendText(json_text); }

  bool SendPing(std::string_view payload = {}) override {
    return SendRaw(EncodeWebSocketPing(payload));
  }

  bool SendPong(std::string_view payload = {}) override {
    return SendRaw(EncodeWebSocketPong(payload));
  }

  void Close(std::uint16_t status_code = 1000, std::string_view reason = {}) override {
    CloseInternal(status_code, reason, true);
  }

  [[nodiscard]] bool IsOpen() const override { return !closed_.load(); }

  void
  SetOnMessage(std::function<void(std::string_view payload, bool is_binary)> callback) override {
    const std::lock_guard<std::mutex> lock(cb_mutex_);
    on_message_ = std::move(callback);
  }

  void SetOnClose(
      std::function<void(std::uint16_t status_code, std::string_view reason)> callback) override {
    const std::lock_guard<std::mutex> lock(cb_mutex_);
    on_close_ = std::move(callback);
  }

  void SetOnError(std::function<void(std::string_view error)> callback) override {
    const std::lock_guard<std::mutex> lock(cb_mutex_);
    on_error_ = std::move(callback);
  }

  void RunDuplexLoop(std::size_t max_ws_payload_bytes, std::string initial_buffer = {}) {
    std::string buffer = std::move(initial_buffer);
    char chunk[4096];
    WsFragmentState fragment;

    while (!closed_.load()) {
      while (!buffer.empty()) {
        WsFrame frame;
        std::size_t consumed = 0;
        std::string err;
        const auto status =
            DecodeWebSocketFrame(buffer, &frame, &consumed, max_ws_payload_bytes, &err);

        if (status == WsDecodeStatus::kNeedMoreData) {
          break;
        }

        if (status != WsDecodeStatus::kOk) {
          DispatchError(err);
          const std::uint16_t code = (status == WsDecodeStatus::kPayloadTooLarge) ? 1009 : 1002;
          CloseInternal(code, err, true);
          return;
        }

        buffer.erase(0, consumed);

        if (IsControlOpcode(frame.header.opcode)) {
          if (!HandleControlFrame(frame)) {
            return;
          }
        } else {
          if (!HandleDataFrame(frame, &fragment, max_ws_payload_bytes)) {
            return;
          }
        }
      }

      const ssize_t n = recv(read_fd_, chunk, sizeof(chunk), 0);
      if (n > 0) {
        buffer.append(chunk, static_cast<std::size_t>(n));
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }

    CloseInternal(1006, "Connection closed", false);
  }

private:
  bool HandleControlFrame(const WsFrame& frame) {
    if (!frame.header.masked) {
      CloseInternal(1002, "Client control frames must be masked", true);
      return false;
    }
    if (frame.header.opcode == WsOpcode::kPing) {
      SendPong(frame.payload);
      return true;
    }
    if (frame.header.opcode == WsOpcode::kClose) {
      std::uint16_t code = 1000;
      std::string reason;
      if (frame.payload.size() >= 2) {
        code = (static_cast<std::uint8_t>(frame.payload[0]) << 8) |
               static_cast<std::uint8_t>(frame.payload[1]);
        reason = frame.payload.substr(2);
      }
      CloseInternal(code, reason, true);
      return false;
    }
    return true;
  }

  bool HandleDataFrame(const WsFrame& frame, WsFragmentState* fragment,
                       std::size_t max_ws_payload_bytes) {
    if (!frame.header.masked) {
      CloseInternal(1002, "Client data frames must be masked", true);
      return false;
    }

    if (frame.header.opcode == WsOpcode::kContinuation) {
      if (!fragment->in_fragment) {
        CloseInternal(1002, "Unexpected continuation frame", true);
        return false;
      }
    } else {
      if (fragment->in_fragment) {
        CloseInternal(1002, "Expected continuation frame", true);
        return false;
      }
    }

    const std::size_t new_size = fragment->buffer.size() + frame.payload.size();
    if (new_size > max_ws_payload_bytes) {
      CloseInternal(1009, "Message payload exceeds maximum limit", true);
      return false;
    }

    if (!frame.header.fin) {
      if (!fragment->in_fragment) {
        fragment->in_fragment = true;
        fragment->opcode = frame.header.opcode;
        fragment->buffer = frame.payload;
      } else {
        fragment->buffer.append(frame.payload);
      }
      return true;
    }

    if (fragment->in_fragment) {
      fragment->buffer.append(frame.payload);
      if (fragment->opcode == WsOpcode::kText && !vinput::str::IsValidUtf8(fragment->buffer)) {
        CloseInternal(1007, "Invalid UTF-8 in fragmented text frame", true);
        return false;
      }
      DispatchMessage(fragment->buffer, fragment->opcode == WsOpcode::kBinary);
      fragment->in_fragment = false;
      fragment->buffer.clear();
      return true;
    }

    if (frame.header.opcode == WsOpcode::kText && !vinput::str::IsValidUtf8(frame.payload)) {
      CloseInternal(1007, "Invalid UTF-8 in text frame", true);
      return false;
    }

    DispatchMessage(frame.payload, frame.header.opcode == WsOpcode::kBinary);
    return true;
  }

  bool SendRaw(std::string_view frame_bytes) {
    if (closed_.load()) {
      return false;
    }
    const std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_.load() || fd_ < 0) {
      return false;
    }
    return WriteAll(fd_, frame_bytes);
  }

  void CloseInternal(std::uint16_t status_code, std::string_view reason, bool send_close_frame) {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
      return;
    }

    int fd_to_close = -1;
    if (send_close_frame) {
      const std::string close_frame = EncodeWebSocketClose(status_code, reason);
      const std::lock_guard<std::mutex> lock(send_mutex_);
      if (fd_ >= 0) {
        (void)WriteAll(fd_, close_frame);
      }
      fd_to_close = fd_;
      fd_ = -1;
    } else {
      const std::lock_guard<std::mutex> lock(send_mutex_);
      fd_to_close = fd_;
      fd_ = -1;
    }

    if (fd_to_close >= 0) {
      shutdown(fd_to_close, SHUT_RDWR);
    }

    DispatchClose(status_code, reason);
  }

  void DispatchMessage(std::string_view payload, bool is_binary) {
    std::function<void(std::string_view, bool)> cb;
    {
      const std::lock_guard<std::mutex> lock(cb_mutex_);
      cb = on_message_;
    }
    if (cb) {
      cb(payload, is_binary);
    }
  }

  void DispatchClose(std::uint16_t code, std::string_view reason) {
    std::function<void(std::uint16_t, std::string_view)> cb;
    {
      const std::lock_guard<std::mutex> lock(cb_mutex_);
      cb = on_close_;
    }
    if (cb) {
      cb(code, reason);
    }
  }

  void DispatchError(std::string_view error) {
    std::function<void(std::string_view)> cb;
    {
      const std::lock_guard<std::mutex> lock(cb_mutex_);
      cb = on_error_;
    }
    if (cb) {
      cb(error);
    }
  }

  const int read_fd_ = -1;
  int fd_ = -1;
  std::atomic<bool> closed_{false};
  std::mutex send_mutex_;
  std::mutex cb_mutex_;
  std::function<void(std::string_view, bool)> on_message_;
  std::function<void(std::uint16_t, std::string_view)> on_close_;
  std::function<void(std::string_view)> on_error_;
};

} // namespace

HttpServer::HttpServer(HttpServerConfig config) : config_(std::move(config)) {}

HttpServer::~HttpServer() {
  Stop();
}

void HttpServer::Get(std::string path, HttpHandler handler) {
  Route(HttpMethod::Get, std::move(path), std::move(handler));
}

void HttpServer::Post(std::string path, HttpHandler handler) {
  Route(HttpMethod::Post, std::move(path), std::move(handler));
}

void HttpServer::Route(HttpMethod method, std::string path, HttpHandler handler) {
  const std::lock_guard<std::mutex> lock(routes_mutex_);
  routes_[std::move(path)][method] = std::move(handler);
}

void HttpServer::UpgradeWebSocket(std::string path, WebSocketHandler handler,
                                  WebSocketAuthorizer authorizer) {
  const std::lock_guard<std::mutex> lock(routes_mutex_);
  ws_endpoints_[std::move(path)] = WebSocketEndpoint{
      .handler = std::move(handler),
      .authorizer = std::move(authorizer),
  };
}

void HttpServer::UpgradeWebSocket(std::string path,
                                  std::function<void(std::shared_ptr<WebSocketSession>)> on_connect,
                                  std::function<void(std::string_view, bool)> on_message,
                                  std::function<void(std::uint16_t, std::string_view)> on_close,
                                  std::function<void(std::string_view)> on_error) {
  UpgradeWebSocket(
      std::move(path),
      [on_connect = std::move(on_connect), on_message = std::move(on_message),
       on_close = std::move(on_close), on_error = std::move(on_error)](
          const std::shared_ptr<WebSocketSession>& session, const HttpRequest& /*req*/) {
        if (on_message) {
          session->SetOnMessage(on_message);
        }
        if (on_close) {
          session->SetOnClose(on_close);
        }
        if (on_error) {
          session->SetOnError(on_error);
        }
        if (on_connect) {
          on_connect(session);
        }
      });
}

bool HttpServer::Start() {
  if (running_.load()) {
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  const char* node = config_.host.empty() ? nullptr : config_.host.c_str();
  const std::string port_str = std::to_string(config_.port);
  struct addrinfo* res = nullptr;
  const int gai_rc = getaddrinfo(node, port_str.c_str(), &hints, &res);
  if (gai_rc != 0 || res == nullptr) {
    return false;
  }

  int listen_fd = -1;
  for (auto* p = res; p != nullptr; p = p->ai_next) {
    listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (listen_fd < 0) {
      continue;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    fcntl(listen_fd, F_SETFD, FD_CLOEXEC);
    if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == 0) {
      break;
    }
    close(listen_fd);
    listen_fd = -1;
  }
  freeaddrinfo(res);

  if (listen_fd < 0) {
    return false;
  }

  if (listen(listen_fd, 64) != 0) {
    close(listen_fd);
    return false;
  }

  if (pipe(stop_pipe_) != 0) {
    close(listen_fd);
    return false;
  }
  fcntl(stop_pipe_[0], F_SETFD, FD_CLOEXEC);
  fcntl(stop_pipe_[1], F_SETFD, FD_CLOEXEC);
  const int pflags = fcntl(stop_pipe_[0], F_GETFL, 0);
  fcntl(stop_pipe_[0], F_SETFL, pflags | O_NONBLOCK);

  sockaddr sa_buf[2]{};
  socklen_t slen = sizeof(sa_buf);
  if (getsockname(listen_fd, &sa_buf[0], &slen) == 0) {
    char host_buf[NI_MAXHOST];
    char serv_buf[NI_MAXSERV];
    if (getnameinfo(&sa_buf[0], slen, host_buf, sizeof(host_buf), serv_buf, sizeof(serv_buf),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
      bound_address_ = host_buf;
      bound_port_ = static_cast<std::uint16_t>(std::atoi(serv_buf));
    }
  }

  listen_fd_ = listen_fd;
  running_.store(true);
  accept_thread_ = std::thread([this]() { AcceptLoop(); });
  return true;
}

void HttpServer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (stop_pipe_[1] >= 0) {
    char byte = 'q';
    (void)write(stop_pipe_[1], &byte, 1);
  }

  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }

  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  if (stop_pipe_[0] >= 0) {
    close(stop_pipe_[0]);
    stop_pipe_[0] = -1;
  }
  if (stop_pipe_[1] >= 0) {
    close(stop_pipe_[1]);
    stop_pipe_[1] = -1;
  }

  const std::thread::id current_id = std::this_thread::get_id();
  std::vector<std::shared_ptr<ClientWorker>> to_join;
  {
    const std::lock_guard<std::mutex> lock(workers_mutex_);
    for (const auto& worker : workers_) {
      const int fd = worker->fd.load();
      if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
      }
    }
    to_join = workers_;
    workers_.clear();
  }

  for (auto& worker : to_join) {
    if (worker->thread.joinable()) {
      if (worker->thread.get_id() == current_id) {
        worker->thread.detach();
      } else {
        worker->thread.join();
      }
    }
  }
}

bool HttpServer::IsRunning() const {
  return running_.load();
}

std::uint16_t HttpServer::BoundPort() const {
  return bound_port_;
}

std::string HttpServer::BoundAddress() const {
  return bound_address_;
}

void HttpServer::PruneFinishedWorkers() {
  const std::lock_guard<std::mutex> lock(workers_mutex_);
  for (auto it = workers_.begin(); it != workers_.end();) {
    if ((*it)->done.load()) {
      if ((*it)->thread.joinable()) {
        (*it)->thread.join();
      }
      it = workers_.erase(it);
    } else {
      ++it;
    }
  }
}

void HttpServer::AcceptLoop() {
  while (running_.load()) {
    PruneFinishedWorkers();

    pollfd pfd[2];
    pfd[0].fd = listen_fd_;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = stop_pipe_[0];
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;

    const int ret = poll(pfd, 2, -1);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((pfd[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
      break;
    }
    if ((pfd[0].revents & POLLIN) != 0) {
      const int client_fd = accept(listen_fd_, nullptr, nullptr);
      if (client_fd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        if (!running_.load()) {
          break;
        }
        continue;
      }
      fcntl(client_fd, F_SETFD, FD_CLOEXEC);

      // Concurrency limit guard
      {
        const std::lock_guard<std::mutex> lock(workers_mutex_);
        if (workers_.size() >= config_.max_connections) {
          SendErrorResponse(client_fd, 503, "Service Unavailable: connection limit reached\n");
          close(client_fd);
          continue;
        }
      }

      auto worker = std::make_shared<ClientWorker>();
      worker->fd.store(client_fd);
      worker->thread = std::thread([this, worker]() {
        HandleClient(worker);
        const int fd = worker->fd.exchange(-1);
        if (fd >= 0) {
          close(fd);
        }
        worker->done.store(true);
      });

      const std::lock_guard<std::mutex> lock(workers_mutex_);
      workers_.push_back(std::move(worker));
    }
  }
}

void HttpServer::HandleClient(const std::shared_ptr<ClientWorker>& worker) {
  const int fd = worker->fd.load();
  if (fd < 0) {
    return;
  }

  bool upgraded = false;
  try {
    const int timeout_ms = config_.read_timeout_seconds * 1000;
    std::string buffer;
    if (!ReadHttpHeaders(fd, &buffer, kMaxHeaderBytes, timeout_ms)) {
      return;
    }

    HttpRequest req;
    std::string body;
    if (!ParseHttpRequestHeader(buffer, &req, &body)) {
      SendErrorResponse(fd, 400, "Bad Request\n");
      return;
    }

    sockaddr sa_peer[2]{};
    socklen_t peer_addr_len = sizeof(sa_peer);
    if (getpeername(fd, &sa_peer[0], &peer_addr_len) == 0) {
      char ip_buf[NI_MAXHOST] = {};
      if (getnameinfo(&sa_peer[0], peer_addr_len, ip_buf, sizeof(ip_buf), nullptr, 0,
                      NI_NUMERICHOST) == 0) {
        req.client_ip = ip_buf;
      }
    }

    const auto content_length_opt = ExtractContentLength(req.headers);
    if (!content_length_opt.has_value()) {
      SendErrorResponse(fd, 400, "Bad Request: Malformed Content-Length\n");
      return;
    }

    const std::size_t content_length = *content_length_opt;
    if (content_length > config_.max_request_body_bytes) {
      SendErrorResponse(fd, 413, "Payload Too Large\n");
      return;
    }

    if (!ReadHttpRequestBody(fd, &body, content_length, timeout_ms)) {
      return;
    }
    req.body = std::move(body);

    const std::string path = req.url.substr(0, req.url.find('?'));

    // Route Guard 1: WebSocket Upgrade
    if (IsWebSocketUpgradeRequest(req.headers)) {
      if (req.method != HttpMethod::Get) {
        SendErrorResponse(fd, 405, "Method Not Allowed: WebSocket upgrade requires GET\n",
                          {{"Allow", "GET"}});
        return;
      }

      const auto version = FindHeader(req.headers, "Sec-WebSocket-Version");
      if (!version || *version != "13") {
        SendErrorResponse(fd, 426, "Upgrade Required: Sec-WebSocket-Version must be 13\n",
                          {{"Sec-WebSocket-Version", "13"}});
        return;
      }

      WebSocketEndpoint endpoint;
      {
        const std::lock_guard<std::mutex> lock(routes_mutex_);
        const auto ws_it = ws_endpoints_.find(path);
        if (ws_it == ws_endpoints_.end()) {
          SendErrorResponse(fd, 404, "Not Found\n");
          return;
        }
        endpoint = ws_it->second;
      }

      if (endpoint.authorizer) {
        const auto auth_resp = endpoint.authorizer(req);
        if (auth_resp.has_value()) {
          SendHttpResponse(fd, *auth_resp, req.method);
          return;
        }
      }

      const auto sec_key = FindHeader(req.headers, "Sec-WebSocket-Key");
      if (!sec_key || sec_key->empty()) {
        SendErrorResponse(fd, 400, "Missing Sec-WebSocket-Key\n");
        return;
      }

      const auto accept_val = ComputeWebSocketAccept(*sec_key);
      if (!accept_val) {
        SendErrorResponse(fd, 500, "Failed to compute WebSocket accept key\n");
        return;
      }

      std::ostringstream handshake;
      handshake << "HTTP/1.1 101 Switching Protocols\r\n"
                << "Upgrade: websocket\r\n"
                << "Connection: Upgrade\r\n"
                << "Sec-WebSocket-Accept: " << *accept_val << "\r\n\r\n";
      if (!WriteAll(fd, handshake.str())) {
        return;
      }

      upgraded = true;
      auto session = std::make_shared<WebSocketSessionImpl>(fd);
      try {
        if (endpoint.handler) {
          endpoint.handler(session, req);
        }
        session->RunDuplexLoop(config_.max_ws_payload_bytes, std::move(req.body));
      } catch (...) {
        session->Close(1011, "Internal error");
        throw;
      }
      return;
    }

    // Route Guard 2: Standard HTTP Routing
    HttpHandler matched_handler;
    std::vector<HttpMethod> allowed_methods;

    {
      const std::lock_guard<std::mutex> lock(routes_mutex_);
      const auto it = routes_.find(path);
      if (it == routes_.end()) {
        SendErrorResponse(fd, 404, "Not Found\n");
        return;
      }
      const auto method_it = it->second.find(req.method);
      if (method_it == it->second.end()) {
        for (const auto& [m, _] : it->second) {
          allowed_methods.push_back(m);
        }
      } else {
        matched_handler = method_it->second;
      }
    }

    if (!matched_handler) {
      std::string allow;
      for (std::size_t i = 0; i < allowed_methods.size(); ++i) {
        if (i > 0) {
          allow += ", ";
        }
        allow += MethodToString(allowed_methods[i]);
      }
      SendErrorResponse(fd, 405, "Method Not Allowed\n", {{"Allow", allow}});
      return;
    }

    const HttpResponse resp = matched_handler(req);
    SendHttpResponse(fd, resp, req.method);
  } catch (const std::exception& e) {
    if (!upgraded) {
      SendErrorResponse(fd, 500, "Internal Server Error\n");
    }
  } catch (...) {
    if (!upgraded) {
      SendErrorResponse(fd, 500, "Internal Server Error\n");
    }
  }
}

} // namespace vinput::net
