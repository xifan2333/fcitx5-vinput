#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct CoreConfig;

namespace vinput::net {
class HttpServer;
class WebSocketSession;
} // namespace vinput::net

namespace vinput::daemon::remote {

class RemoteTextService {
public:
  RemoteTextService();
  ~RemoteTextService();

  RemoteTextService(const RemoteTextService&) = delete;
  RemoteTextService& operator=(const RemoteTextService&) = delete;
  RemoteTextService(RemoteTextService&&) = delete;
  RemoteTextService& operator=(RemoteTextService&&) = delete;

  bool Synchronize(const CoreConfig& config, std::string* error);
  void Shutdown();

  [[nodiscard]] std::vector<std::string> ListEndpoints() const;

private:
  struct Settings {
    int port = 8080;
    int debounce_ms = 1500;
    std::string api_key;
  };

  bool ExtractSettings(const CoreConfig& config, Settings* settings, bool* should_run,
                       std::string* error) const;
  bool Start(const Settings& settings, std::string* error);
  void SetupRoutes(vinput::net::HttpServer* server);

  void HandleOutputSession(const std::shared_ptr<vinput::net::WebSocketSession>& session);
  void HandleInputSession(const std::shared_ptr<vinput::net::WebSocketSession>& session);

  [[nodiscard]] bool ValidateApiKey(std::string_view api_key) const;
  void NotifyInput(std::string_view type);

  void SetText(std::string text);
  [[nodiscard]] std::string GetText() const;
  void ClearText();
  void ScheduleDebounce();
  void CancelDebounce();
  void ForceDebounce();
  void DebounceLoop();
  static void SendEmptyCommit(const std::shared_ptr<vinput::net::WebSocketSession>& session);
  void SendFinalResult();

  Settings settings_;
  mutable std::mutex lifecycle_mutex_;
  std::atomic<bool> running_{false};
  bool service_started_ = false;

  std::unique_ptr<vinput::net::HttpServer> http_server_;
  std::thread debounce_thread_;

  mutable std::mutex state_mutex_;
  std::shared_ptr<vinput::net::WebSocketSession> input_session_;
  std::shared_ptr<vinput::net::WebSocketSession> output_session_;

  mutable std::mutex text_mutex_;
  std::string current_text_;

  std::mutex debounce_mutex_;
  std::condition_variable debounce_cv_;
  bool debounce_pending_ = false;
  bool debounce_stop_ = false;
  std::chrono::steady_clock::time_point debounce_deadline_{};
};

} // namespace vinput::daemon::remote
