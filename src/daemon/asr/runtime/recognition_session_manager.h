#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
  // Creates a session from the active backend. Backends in Chunked delivery
  // mode come back pre-wrapped in AsyncRecognitionSession, whose worker thread
  // keeps the owning backend alive and signals wake_callback on every event
  // batch (Issue #709). Buffered backends return direct self-contained sessions.
  std::unique_ptr<RecognitionSession>
  CreateSession(const CoreConfig& settings, BackendDescriptor* descriptor, std::string* error,
                const std::function<void()>& wake_callback = {});
  ReloadSnapshot GetReloadSnapshot() const;
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
                                         std::string* error,
                                         const std::function<void()>& wake_callback);
  void EnsureReloadWorkerStarted();
  void ReloadWorkerMain();
  void ApplyPreparedBackendLocked(PreparedBackend prepared, bool sync_targets);
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
  std::optional<PreparedBackend> effective_backend_;
  std::string last_reload_error_;
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
  std::thread reload_worker_;
  std::function<void(bool success, const std::string& message)> reload_result_callback_;
};

} // namespace vinput::daemon::asr
