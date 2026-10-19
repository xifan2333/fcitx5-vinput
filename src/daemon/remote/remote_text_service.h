#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct CoreConfig;

namespace httplib {
class Server;
namespace ws {
class WebSocket;
}
} // namespace httplib

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

  using StartCallback = std::function<bool()>;
  using StopCallback = std::function<bool(const std::string& scene_id)>;

  void SetSessionControlCallbacks(StartCallback on_start, StopCallback on_stop);

  [[nodiscard]] std::vector<std::string> ListEndpoints() const;
  [[nodiscard]] std::string GetText() const;
  [[nodiscard]] int Port() const;

private:
  struct Settings {
    int port = 8080;
    int debounce_ms = 1500;
    std::string api_key;
  };

  bool ExtractSettings(const CoreConfig& config, Settings* settings, bool* should_run,
                       std::string* error) const;
  bool Start(const Settings& settings, std::string* error);
  void SetupRoutes(httplib::Server* server);

  void HandleOutputSession(httplib::ws::WebSocket& ws);
  void HandleInputSession(httplib::ws::WebSocket& ws);

  [[nodiscard]] bool ValidateApiKey(std::string_view api_key) const;
  void NotifyInput(std::string_view type);

  void SetText(std::string text);
  void ClearText();
  void ScheduleDebounce();
  void CancelDebounce();
  void ForceDebounce();
  void DebounceLoop();
  void MaybeTriggerStart();
  static void SendEmptyCommit(httplib::ws::WebSocket& ws);
  void SendFinalResult();

  Settings settings_;
  int bound_port_ = 0;
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex callback_mutex_;
  StartCallback on_start_;
  StopCallback on_stop_;
  std::atomic<bool> start_in_progress_{false};
  std::atomic<bool> running_{false};
  bool service_started_ = false;

  std::unique_ptr<httplib::Server> http_server_;
  std::thread server_thread_;
  std::thread debounce_thread_;

  mutable std::mutex state_mutex_;
  httplib::ws::WebSocket* input_ws_ = nullptr;
  httplib::ws::WebSocket* output_ws_ = nullptr;

  mutable std::mutex text_mutex_;
  std::string current_text_;

  std::mutex debounce_mutex_;
  std::condition_variable debounce_cv_;
  bool debounce_pending_ = false;
  bool debounce_stop_ = false;
  std::chrono::steady_clock::time_point debounce_deadline_{};
};

} // namespace vinput::daemon::remote
