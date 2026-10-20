#include "daemon/remote/remote_text_service.h"

#include <algorithm>
#include <asm-generic/socket.h>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <httplib.h>
#include <ifaddrs.h>
#include <map>
#include <memory>
#include <mutex>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/config/core_config.h"
#include "common/utils/interpolation.h"
#include "common/utils/string_utils.h"

#include "daemon/remote/remote_assets.h"

namespace vinput::daemon::remote {

namespace {

using json = nlohmann::json;

constexpr std::string_view kRemoteProviderId = "provider.vinput.remote.streaming";

std::optional<int> ParsePort(std::string_view value) {
  const std::string_view trimmed = vinput::str::Trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  int parsed = 0;
  const char* begin = trimmed.data();
  const char* end = begin + trimmed.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed < 0 || parsed > 65535) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<int> ParsePositiveInt(std::string_view value) {
  const std::string_view trimmed = vinput::str::Trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  int parsed = 0;
  const char* begin = trimmed.data();
  const char* end = begin + trimmed.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed <= 0) {
    return std::nullopt;
  }
  return parsed;
}

std::string EnvValue(const std::map<std::string, std::string>& env, std::string_view key) {
  const auto it = env.find(std::string(key));
  if (it == env.end()) {
    return {};
  }
  return std::string(vinput::str::Trim(it->second));
}

std::optional<int> ParsePortFromWsUrl(std::string_view url) {
  const std::string_view text = vinput::str::Trim(url);
  const std::size_t scheme = text.find("://");
  if (scheme == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t authority_begin = scheme + 3;
  std::size_t authority_end = text.find('/', authority_begin);
  if (authority_end == std::string_view::npos) {
    authority_end = text.size();
  }
  const std::string_view authority = text.substr(authority_begin, authority_end - authority_begin);
  if (authority.empty()) {
    return std::nullopt;
  }
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos || close + 1 >= authority.size() ||
        authority[close + 1] != ':') {
      return std::nullopt;
    }
    return ParsePort(authority.substr(close + 2));
  }
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos || colon + 1 >= authority.size()) {
    return std::nullopt;
  }
  return ParsePort(authority.substr(colon + 1));
}

std::string NewEventId(std::string_view prefix) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return std::string(prefix) + "_" + std::to_string(ns);
}

bool IsLoopbackAddress(std::string_view ip) {
  if (ip == "127.0.0.1" || ip == "::1" || ip == "::ffff:127.0.0.1") {
    return true;
  }
  return ip.starts_with("127.");
}

std::string ExtractBearerToken(std::string_view auth_header) {
  const std::string_view trimmed = vinput::str::Trim(auth_header);
  if (!vinput::str::StartsWithIgnoreCase(trimmed, "Bearer ")) {
    return {};
  }
  return std::string(vinput::str::Trim(trimmed.substr(7)));
}

} // namespace

RemoteTextService::RemoteTextService() = default;

RemoteTextService::~RemoteTextService() {
  Shutdown();
}

int RemoteTextService::Port() const {
  const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  return bound_port_ > 0 ? bound_port_ : settings_.port;
}

bool RemoteTextService::ExtractSettings(const CoreConfig& config, Settings* settings,
                                        bool* should_run, std::string* error) const {
  if (!settings) {
    if (error != nullptr) {
      *error = "settings output is null";
    }
    return false;
  }
  if (should_run) {
    *should_run = false;
  }

  const AsrProvider* provider = ResolveActiveAsrProvider(config);
  const auto* command = provider ? std::get_if<CommandAsrProvider>(provider) : nullptr;
  if (!command) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const bool is_remote = command->id == kRemoteProviderId;
  if (!is_remote) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  Settings out;
  const std::string port_text = EnvValue(command->env, "VINPUT_ASR_PORT");
  if (!port_text.empty()) {
    const auto port = ParsePort(port_text);
    if (!port) {
      if (error != nullptr) {
        *error = "VINPUT_ASR_PORT must be between 0 and 65535.";
      }
      return false;
    }
    out.port = *port;
  } else if (const auto port = ParsePortFromWsUrl(EnvValue(command->env, "VINPUT_ASR_URL"))) {
    out.port = *port;
  }

  const std::string debounce_text = EnvValue(command->env, "VINPUT_ASR_DEBOUNCE_MS");
  if (!debounce_text.empty()) {
    const auto debounce = ParsePositiveInt(debounce_text);
    if (!debounce) {
      if (error != nullptr) {
        *error = "VINPUT_ASR_DEBOUNCE_MS must be a positive integer.";
      }
      return false;
    }
    out.debounce_ms = *debounce;
  }

  out.api_key = vinput::interpolate::InterpolateEnv(EnvValue(command->env, "VINPUT_ASR_API_KEY"));
  if (out.api_key.empty()) {
    if (error != nullptr) {
      *error = "Remote ASR provider requires VINPUT_ASR_API_KEY.";
    }
    return false;
  }

  *settings = std::move(out);
  if (should_run) {
    *should_run = true;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool RemoteTextService::Synchronize(const CoreConfig& config, std::string* error) {
  Settings next_settings;
  bool should_run = false;
  if (!ExtractSettings(config, &next_settings, &should_run, error)) {
    Shutdown();
    return false;
  }

  const std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!should_run) {
    if (service_started_) {
      Shutdown();
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const bool same_settings = service_started_ && settings_.port == next_settings.port &&
                             settings_.debounce_ms == next_settings.debounce_ms &&
                             settings_.api_key == next_settings.api_key;
  if (same_settings) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  if (service_started_) {
    Shutdown();
  }
  return Start(next_settings, error);
}

void RemoteTextService::SetSessionControlCallbacks(StartCallback on_start, StopCallback on_stop) {
  const std::lock_guard<std::mutex> lock(callback_mutex_);
  on_start_ = std::move(on_start);
  on_stop_ = std::move(on_stop);
}

void RemoteTextService::MaybeTriggerStart() {
  StartCallback cb;
  {
    const std::lock_guard<std::mutex> lock(callback_mutex_);
    cb = on_start_;
  }
  if (!cb) {
    return;
  }
  bool needs_start = false;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (output_ws_ == nullptr || !output_ws_->is_open()) {
      needs_start = true;
    }
  }
  if (needs_start) {
    bool expected = false;
    if (start_in_progress_.compare_exchange_strong(expected, true)) {
      cb();
    }
  }
}

std::vector<std::string> RemoteTextService::ListEndpoints() const {
  const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (!service_started_) {
    return {};
  }

  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) {
    return {};
  }

  std::vector<std::string> endpoints;
  for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    char buf[NI_MAXHOST] = {};
    if (getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), buf, sizeof(buf), nullptr, 0,
                    NI_NUMERICHOST) != 0) {
      continue;
    }
    endpoints.emplace_back(std::string("http://") + buf + ":" +
                           std::to_string(bound_port_ > 0 ? bound_port_ : settings_.port));
  }
  freeifaddrs(ifaddr);

  std::sort(endpoints.begin(), endpoints.end());
  endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
  return endpoints;
}

void RemoteTextService::SetupRoutes(httplib::Server* server) {
  server->Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(LoadRemoteIndexHtml(), "text/html; charset=utf-8");
  });

  server->Get("/favicon.svg", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(LoadRemoteFaviconSvg(), "image/svg+xml");
  });

  server->Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
  });

  // Pre-request handler for /v1/realtime local loopback and Bearer auth verification
  server->set_pre_request_handler([this](const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/v1/realtime") {
      if (!IsLoopbackAddress(req.remote_addr)) {
        res.status = 403;
        res.set_content("Realtime endpoint is local-only.\n", "text/plain; charset=utf-8");
        return httplib::Server::HandlerResponse::Handled;
      }

      const auto auth = req.get_header_value("Authorization");
      if (auth.empty() || !ValidateApiKey(ExtractBearerToken(auth))) {
        res.status = 401;
        res.set_content("Unauthorized.\n", "text/plain; charset=utf-8");
        return httplib::Server::HandlerResponse::Handled;
      }
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  // WebSocket Output: /v1/realtime (local-only, Bearer auth)
  server->WebSocket("/v1/realtime", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
    HandleOutputSession(ws);
  });

  // WebSocket Input: /ws (in-band auth via first message)
  server->WebSocket("/ws", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
    HandleInputSession(ws);
  });
}

bool RemoteTextService::Start(const Settings& settings, std::string* error) {
  settings_ = settings;

  auto server = std::make_unique<httplib::Server>();

  server->set_socket_options([](socket_t sock) {
#ifdef _WIN32
    httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
  });

  server->set_payload_max_length(2ULL * 1024ULL * 1024ULL);
  server->set_read_timeout(15);

  SetupRoutes(server.get());

  if (settings_.port == 0) {
    const int p = server->bind_to_any_port("0.0.0.0");
    if (p <= 0) {
      if (error != nullptr) {
        *error = "failed to bind remote ASR service on ephemeral port";
      }
      return false;
    }
    bound_port_ = p;
  } else {
    if (!server->bind_to_port("0.0.0.0", settings_.port)) {
      if (error != nullptr) {
        *error = "failed to bind remote ASR service on 0.0.0.0:" + std::to_string(settings_.port);
      }
      return false;
    }
    bound_port_ = settings_.port;
  }

  running_.store(true, std::memory_order_release);
  debounce_pending_ = false;
  debounce_stop_ = false;

  try {
    debounce_thread_ = std::thread([this]() { DebounceLoop(); });
  } catch (const std::exception& e) {
    running_.store(false, std::memory_order_release);
    server->stop();
    if (error != nullptr) {
      *error = std::string("failed to start remote ASR debounce thread: ") + e.what();
    }
    return false;
  }

  http_server_ = std::move(server);
  server_thread_ = std::thread([this]() { http_server_->listen_after_bind(); });
  http_server_->wait_until_ready();

  service_started_ = true;
  std::fprintf(stderr, "vinput-daemon: remote ASR service listening on 0.0.0.0:%d\n",
               settings_.port);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void RemoteTextService::Shutdown() {
  const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
  start_in_progress_.store(false, std::memory_order_release);
  {
    const std::lock_guard<std::mutex> lock(debounce_mutex_);
    debounce_stop_ = true;
    debounce_pending_ = false;
  }
  debounce_cv_.notify_all();

  if (debounce_thread_.joinable()) {
    debounce_thread_.join();
  }

  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (output_ws_ != nullptr && output_ws_->is_open()) {
      output_ws_->close(httplib::ws::CloseStatus::Normal, "Service shutting down");
    }
    if (input_ws_ != nullptr && input_ws_->is_open()) {
      input_ws_->close(httplib::ws::CloseStatus::Normal, "Service shutting down");
    }
  }

  if (http_server_) {
    http_server_->stop();
  }
  if (server_thread_.joinable()) {
    server_thread_.join();
  }
  http_server_.reset();

  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    output_ws_ = nullptr;
    input_ws_ = nullptr;
  }

  if (was_running) {
    ClearText();
  }
  bound_port_ = 0;
  service_started_ = false;
}

void RemoteTextService::HandleOutputSession(httplib::ws::WebSocket& ws) {
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (output_ws_ != nullptr && output_ws_->is_open()) {
      ws.send(json{{"event_id", NewEventId("event")},
                   {"type", "error"},
                   {"error", json{{"message", "Output client already connected."}}}}
                  .dump());
      ws.close(httplib::ws::CloseStatus::PolicyViolation, "Output client already connected.");
      return;
    }
    output_ws_ = &ws;
  }

  NotifyInput("output_connected");

  std::string payload;
  while (ws.is_open()) {
    const auto ret = ws.read(payload);
    if (ret == httplib::ws::ReadResult::Fail) {
      break;
    }
    if (ret != httplib::ws::ReadResult::Text) {
      continue;
    }

    json event;
    try {
      event = json::parse(payload);
    } catch (const std::exception&) {
      continue;
    }

    const std::string type = event.value("type", std::string{});
    if (type == "session.update") {
      json session_data = json::object();
      if (event.contains("session")) {
        session_data = event["session"];
      }
      ws.send(json{{"event_id", NewEventId("event")},
                   {"type", "session.updated"},
                   {"session", std::move(session_data)}}
                  .dump());
    } else if (type == "input_audio_buffer.append") {
      // Audio frames are ignored as input comes via /ws
    } else if (type == "input_audio_buffer.commit") {
      if (GetText().empty()) {
        SendEmptyCommit(ws);
      } else {
        CancelDebounce();
        SendFinalResult();
      }
    }
  }

  bool was_active = false;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (output_ws_ == &ws) {
      output_ws_ = nullptr;
      was_active = true;
    }
  }
  if (was_active) {
    CancelDebounce();
    ClearText();
    NotifyInput("output_disconnected");
  }
}

void RemoteTextService::HandleInputSession(httplib::ws::WebSocket& ws) {
  bool authenticated = false;
  std::string payload;

  while (ws.is_open()) {
    const auto ret = ws.read(payload);
    if (ret == httplib::ws::ReadResult::Fail) {
      break;
    }
    if (ret != httplib::ws::ReadResult::Text) {
      continue;
    }

    json event;
    try {
      event = json::parse(payload);
    } catch (const std::exception&) {
      if (!authenticated) {
        ws.send(json{{"type", "error"}, {"message", "Invalid JSON."}}.dump());
        ws.close(httplib::ws::CloseStatus::PolicyViolation, "Invalid JSON.");
        return;
      }
      continue;
    }

    if (!authenticated) {
      const std::string type = event.value("type", std::string{});
      const std::string api_key = event.value("api_key", std::string{});
      if (type != "auth" || !ValidateApiKey(api_key)) {
        ws.send(json{{"type", "error"}, {"message", "Unauthorized."}}.dump());
        ws.close(httplib::ws::CloseStatus::PolicyViolation, "Unauthorized.");
        return;
      }

      httplib::ws::WebSocket* old_ws = nullptr;
      {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        if (input_ws_ != nullptr && input_ws_ != &ws) {
          old_ws = input_ws_;
        }
        input_ws_ = &ws;
      }

      if (old_ws != nullptr) {
        CancelDebounce();
        ClearText();
        if (start_in_progress_.load()) {
          StopCallback stop_cb;
          {
            const std::lock_guard<std::mutex> lock(callback_mutex_);
            stop_cb = on_stop_;
          }
          if (stop_cb) {
            (void)stop_cb("");
          }
          start_in_progress_.store(false);
        }
        if (old_ws->is_open()) {
          old_ws->send(
              json{{"type", "displaced"}, {"message", "Replaced by new authenticated session."}}
                  .dump());
          old_ws->close(httplib::ws::CloseStatus::Normal, "Replaced by new authenticated session.");
        }
      }

      ws.send(json{{"type", "auth_ok"}}.dump());
      authenticated = true;

      std::string output_status = "disconnected";
      {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        if (output_ws_ != nullptr && output_ws_->is_open()) {
          output_status = "connected";
        }
      }
      ws.send(json{{"type", "init"}, {"output_status", output_status}}.dump());
      continue;
    }

    {
      const std::lock_guard<std::mutex> lock(state_mutex_);
      if (input_ws_ != &ws) {
        return;
      }
    }

    const std::string type = event.value("type", std::string{});
    if (type == "text_update") {
      const std::string text = event.value("text", std::string{});
      SetText(text);
      if (!text.empty()) {
        MaybeTriggerStart();
      }
      ScheduleDebounce();
    } else if (type == "finalize") {
      ForceDebounce();
    }
  }

  bool was_active = false;
  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (input_ws_ == &ws) {
      input_ws_ = nullptr;
      was_active = true;
    }
  }
  if (was_active) {
    CancelDebounce();
  }
}

bool RemoteTextService::ValidateApiKey(std::string_view api_key) const {
  const std::string expected = settings_.api_key;
  if (expected.empty() || api_key.size() != expected.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    diff |= static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(api_key[i]);
  }
  return diff == 0;
}

void RemoteTextService::NotifyInput(std::string_view type) {
  const std::lock_guard<std::mutex> lock(state_mutex_);
  if (input_ws_ != nullptr && input_ws_->is_open()) {
    input_ws_->send(json{{"type", type}}.dump());
  }
}

void RemoteTextService::SetText(std::string text) {
  const std::lock_guard<std::mutex> lock(text_mutex_);
  current_text_ = std::move(text);
}

std::string RemoteTextService::GetText() const {
  const std::lock_guard<std::mutex> lock(text_mutex_);
  return current_text_;
}

void RemoteTextService::ClearText() {
  const std::lock_guard<std::mutex> lock(text_mutex_);
  current_text_.clear();
}

void RemoteTextService::ScheduleDebounce() {
  {
    const std::lock_guard<std::mutex> lock(debounce_mutex_);
    debounce_pending_ = true;
    debounce_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(settings_.debounce_ms);
  }
  debounce_cv_.notify_all();
}

void RemoteTextService::CancelDebounce() {
  {
    const std::lock_guard<std::mutex> lock(debounce_mutex_);
    debounce_pending_ = false;
  }
  debounce_cv_.notify_all();
}

void RemoteTextService::ForceDebounce() {
  CancelDebounce();
  SendFinalResult();
}

void RemoteTextService::DebounceLoop() {
  std::unique_lock<std::mutex> lock(debounce_mutex_);
  while (!debounce_stop_) {
    debounce_cv_.wait(lock, [this]() { return debounce_stop_ || debounce_pending_; });
    while (!debounce_stop_ && debounce_pending_) {
      const auto deadline = debounce_deadline_;
      if (debounce_cv_.wait_until(lock, deadline, [this, deadline]() {
            return debounce_stop_ || !debounce_pending_ || debounce_deadline_ != deadline;
          })) {
        continue;
      }
      debounce_pending_ = false;
      lock.unlock();
      SendFinalResult();
      lock.lock();
    }
  }
}

void RemoteTextService::SendEmptyCommit(httplib::ws::WebSocket& ws) {
  if (ws.is_open()) {
    ws.send(json{{"event_id", NewEventId("event")},
                 {"type", "input_audio_buffer.committed"},
                 {"item_id", NewEventId("item")}}
                .dump());
  }
}

void RemoteTextService::SendFinalResult() {
  start_in_progress_.store(false, std::memory_order_release);

  const std::string text = GetText();
  if (text.empty()) {
    NotifyInput("committed");
    StopCallback stop_cb;
    {
      const std::lock_guard<std::mutex> lock(callback_mutex_);
      stop_cb = on_stop_;
    }
    if (stop_cb) {
      stop_cb("");
    }
    return;
  }

  {
    const std::lock_guard<std::mutex> lock(state_mutex_);
    if (output_ws_ != nullptr && output_ws_->is_open()) {
      const std::string item_id = NewEventId("item");
      output_ws_->send(json{
          {"event_id", NewEventId("event")},
          {"type", "input_audio_buffer.committed"},
          {"item_id",
           item_id}}.dump());
      output_ws_->send(json{
          {"event_id", NewEventId("event")},
          {"type", "conversation.item.input_audio_transcription.delta"},
          {"item_id", item_id},
          {"delta", text}}.dump());
      output_ws_->send(json{
          {"event_id", NewEventId("event")},
          {"type", "conversation.item.input_audio_transcription.completed"},
          {"item_id", item_id},
          {"transcript",
           text}}.dump());
    }
  }

  ClearText();
  NotifyInput("committed");

  StopCallback stop_cb;
  {
    const std::lock_guard<std::mutex> lock(callback_mutex_);
    stop_cb = on_stop_;
  }
  if (stop_cb) {
    stop_cb("");
  }
}

} // namespace vinput::daemon::remote
