#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/config/core_config_types.h"

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::notify {
class NotificationBridge;
}

namespace vinput::daemon::asr {

struct RecognitionRunResult {
  bool available = false;
  bool ok = true;
  std::string text;
  std::string error;
};

class RecognitionSessionManager {
public:
  struct ReloadSnapshot {
    std::string target_provider_id;
    std::string target_model_id;
    std::string effective_provider_id;
    std::string effective_model_id;
    std::string target_signature;
    std::string effective_signature;
    std::string last_error;
    bool reload_in_progress = false;
    bool has_effective_backend = false;
  };

  explicit RecognitionSessionManager(
      vinput::notify::NotificationBridge* notification_bridge = nullptr);
  ~RecognitionSessionManager();

  RecognitionSessionManager(const RecognitionSessionManager&) = delete;
  RecognitionSessionManager& operator=(const RecognitionSessionManager&) = delete;
  RecognitionSessionManager(RecognitionSessionManager&&) = delete;
  RecognitionSessionManager& operator=(RecognitionSessionManager&&) = delete;

  bool Initialize(const CoreConfig& settings, std::string* disabled_reason);
  bool SynchronizeBackend(const CoreConfig& settings, std::string* error);
  std::unique_ptr<RecognitionSession>
  CreateSession(const CoreConfig& settings, BackendDescriptor* descriptor, std::string* error);
  static RecognitionRunResult ConsumeEvents(std::unique_ptr<RecognitionSession>* session,
                                            bool cancel, std::string* error);
  ReloadSnapshot GetReloadSnapshot() const;
  void SetNotificationBridge(vinput::notify::NotificationBridge* notification_bridge);
  void
  SetReloadResultCallback(std::function<void(bool success, const std::string& message)> callback);
  void Shutdown();

private:
  struct PreparedBackend {
    std::shared_ptr<AsrBackend> backend;
    BackendDescriptor descriptor;
    std::string provider_id;
    std::string model_id;
    std::string signature;
  };

  static bool
  CreatePreparedBackend(const CoreConfig& settings, PreparedBackend* prepared, std::string* error,
                        vinput::notify::NotificationBridge* notification_bridge = nullptr);
  bool ActivatePreparedBackend(PreparedBackend prepared, std::string* error);
  static bool
  CreatePreparedBackendForReload(const CoreConfig& settings, const std::string& signature,
                                 PreparedBackend* prepared, std::string* error,
                                 vinput::notify::NotificationBridge* notification_bridge = nullptr);
  bool CreateSessionFromEffectiveBackend(BackendDescriptor* descriptor,
                                         std::unique_ptr<RecognitionSession>* session,
                                         std::string* error);
  void EnsureReloadWorkerStarted();
  void ReloadWorkerMain();
  void ResetEffectiveBackendLocked();
  void ApplyDisabledStateLocked();
  void NotifyReloadResult(bool success, const std::string& message);

  mutable std::mutex state_mutex_;
  std::condition_variable reload_cv_;
  bool reload_worker_running_ = false;
  bool reload_requested_ = false;
  bool reload_in_progress_ = false;
  CoreConfig pending_settings_;
  std::string target_provider_id_;
  std::string target_model_id_;
  std::string target_backend_signature_;
  std::shared_ptr<AsrBackend> effective_backend_;
  BackendDescriptor effective_descriptor_;
  bool has_effective_descriptor_ = false;
  std::string effective_provider_id_;
  std::string effective_model_id_;
  std::string effective_backend_signature_;
  std::string last_reload_error_;
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
  std::thread reload_worker_;
  std::function<void(bool success, const std::string& message)> reload_result_callback_;
};

} // namespace vinput::daemon::asr
