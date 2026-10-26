#include "daemon/asr/runtime/recognition_session_manager.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <glaze/json/json_t.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/json/json_access.h"
#include "common/utils/debug_log.h"
#include "common/utils/time_utils.h"

#include "daemon/asr/runtime/backend_factory.h"
#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

namespace {

class BackendKeepingSession : public RecognitionSession {
public:
  BackendKeepingSession(std::shared_ptr<AsrBackend> backend,
                        std::unique_ptr<RecognitionSession> session)
      : backend_(std::move(backend)), session_(std::move(session)) {}

  using RecognitionSession::PushAudio;

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    if (!session_) {
      if (error != nullptr) {
        *error = "Recognition session is not initialized.";
      }
      return false;
    }
    return session_->PushAudio(pcm, error);
  }

  bool PushAudio(std::span<const float> samples, std::string* error) override {
    if (!session_) {
      if (error != nullptr) {
        *error = "Recognition session is not initialized.";
      }
      return false;
    }
    return session_->PushAudio(samples, error);
  }

  bool Finish(std::string* error) override {
    if (!session_) {
      if (error != nullptr) {
        *error = "Recognition session is not initialized.";
      }
      return false;
    }
    return session_->Finish(error);
  }

  void Cancel() override {
    if (session_) {
      session_->Cancel();
    }
  }

  void Interrupt() noexcept override {
    if (session_) {
      session_->Interrupt();
    }
  }

  std::vector<RecognitionEvent> PollEvents() override {
    if (!session_) {
      return {};
    }
    return session_->PollEvents();
  }

private:
  std::shared_ptr<AsrBackend> backend_;
  std::unique_ptr<RecognitionSession> session_;
};

bool ShouldDisableAsr(const CoreConfig& config, std::string* reason) {
  const AsrProvider* provider = ResolveActiveAsrProvider(config);
  if (provider == nullptr) {
    if (reason != nullptr) {
      *reason = _("No active ASR provider configured.");
    }
    return true;
  }

  return false;
}

std::string BuildRuntimeSignature(const CoreConfig& config) {
  glz::json_t j = glz::json_t(glz::json_t::object_t{});
  j["active_provider"] = config.asr.activeProvider;

  const AsrProvider* provider = ResolveActiveAsrProvider(config);
  if (provider == nullptr) {
    return vinput::json::ToText(j);
  }

  glz::json_t provider_json = glz::json_t(glz::json_t::object_t{});
  provider_json["id"] = AsrProviderId(*provider);
  provider_json["type"] = std::string(AsrProviderType(*provider));
  provider_json["timeout_ms"] = AsrProviderTimeoutMs(*provider);

  if (const auto* local = std::get_if<LocalAsrProvider>(provider)) {
    provider_json["model"] = local->model;
    provider_json["hotwords_file"] = local->hotwordsFile;
    provider_json["vad_enabled"] = config.asr.vad.enabled;
  } else if (const auto* command = std::get_if<CommandAsrProvider>(provider)) {
    provider_json["command"] = command->command;
    provider_json["args"] = vinput::json::FromStrings(command->args);
    provider_json["env"] = vinput::json::FromStringMap(command->env);
  }

  j["provider"] = std::move(provider_json);
  return vinput::json::ToText(j);
}

void LogActiveBackend(const CoreConfig& config) {
  BackendDescriptor descriptor;
  std::string error;
  if (!DescribeActiveBackend(config, &descriptor, &error)) {
    VLOG_DEBUG("ASR provider=(missing)");
    return;
  }

  VLOG_DEBUG("ASR provider={} type={} backend={}", descriptor.provider_id, descriptor.provider_type,
             descriptor.backend_id);
}

} // namespace

RecognitionSessionManager::RecognitionSessionManager(
    vinput::notify::NotificationBridge* notification_bridge)
    : notification_bridge_(notification_bridge) {}

RecognitionSessionManager::~RecognitionSessionManager() {
  Shutdown();
}

bool RecognitionSessionManager::Initialize(const CoreConfig& settings,
                                           std::string* disabled_reason) {
  std::string disabled_state_reason;
  if (ShouldDisableAsr(settings, &disabled_state_reason)) {
    const std::scoped_lock lock(state_mutex_);
    ApplyDisabledStateLocked();
    if (disabled_reason != nullptr) {
      *disabled_reason = std::move(disabled_state_reason);
    }
    return false;
  }

  PreparedBackend prepared;
  std::string error;
  if (!CreatePreparedBackend(settings, &prepared, &error, notification_bridge_)) {
    {
      const std::scoped_lock lock(state_mutex_);
      ResetEffectiveBackendLocked();
      target_backend_signature_.clear();
      last_reload_error_ = error;
    }
    if (disabled_reason != nullptr) {
      *disabled_reason = error;
    }
    return false;
  }

  if (!ActivatePreparedBackend(std::move(prepared), &error)) {
    if (disabled_reason != nullptr) {
      *disabled_reason = error;
    }
    return false;
  }

  EnsureReloadWorkerStarted();
  if (disabled_reason != nullptr) {
    disabled_reason->clear();
  }
  return true;
}

bool RecognitionSessionManager::SynchronizeBackend(const CoreConfig& settings, std::string* error) {
  std::string disabled_reason;
  if (ShouldDisableAsr(settings, &disabled_reason)) {
    const std::scoped_lock lock(state_mutex_);
    ApplyDisabledStateLocked();
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  EnsureReloadWorkerStarted();

  const std::string signature = BuildRuntimeSignature(settings);
  {
    const std::scoped_lock lock(state_mutex_);
    if (effective_backend_ && effective_backend_signature_ == signature && !reload_in_progress_) {
      const AsrProvider* provider = ResolveActiveAsrProvider(settings);
      target_provider_id_ = provider != nullptr ? AsrProviderId(*provider) : std::string{};
      target_model_id_ = ResolvePreferredLocalModel(settings);
      target_backend_signature_ = signature;
      last_reload_error_.clear();
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    pending_settings_ = settings;
    const AsrProvider* provider = ResolveActiveAsrProvider(settings);
    target_provider_id_ = provider != nullptr ? AsrProviderId(*provider) : std::string{};
    target_model_id_ = ResolvePreferredLocalModel(settings);
    target_backend_signature_ = signature;
    reload_requested_ = true;
    last_reload_error_.clear();
    VLOG_DEBUG("queue ASR backend reload provider={} model={} signature={}",
               target_provider_id_.empty() ? "(none)" : target_provider_id_,
               target_model_id_.empty() ? "(none)" : target_model_id_, target_backend_signature_);
  }

  reload_cv_.notify_one();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::unique_ptr<RecognitionSession>
RecognitionSessionManager::CreateSession(const CoreConfig& settings, BackendDescriptor* descriptor,
                                         std::string* error) {
  (void)settings;

  std::unique_ptr<RecognitionSession> session;
  if (!CreateSessionFromEffectiveBackend(descriptor, &session, error)) {
    return nullptr;
  }

  if (error != nullptr) {
    error->clear();
  }
  return session;
}

RecognitionRunResult
RecognitionSessionManager::ConsumeEvents(std::unique_ptr<RecognitionSession>* session, bool cancel,
                                         std::string* error) {
  RecognitionRunResult result;
  if (session == nullptr || *session == nullptr) {
    if (error != nullptr) {
      *error = "Recognition session is not initialized.";
    }
    result.ok = false;
    result.error = error != nullptr ? *error : "Recognition session is not initialized.";
    return result;
  }

  result.available = true;
  std::string session_error;
  if (cancel) {
    (*session)->Cancel();
  } else if (!(*session)->Finish(&session_error) && !session_error.empty()) {
    result.ok = false;
    result.error = session_error;
  }

  for (auto& event : (*session)->PollEvents()) {
    switch (event.kind) {
    case RecognitionEventKind::PartialText:
      break;
    case RecognitionEventKind::FinalText:
      result.text = std::move(event.text);
      break;
    case RecognitionEventKind::Error:
      result.ok = false;
      result.error = std::move(event.error);
      break;
    case RecognitionEventKind::Completed:
      break;
    }
  }

  session->reset();
  if (error != nullptr) {
    *error = result.error;
  }
  return result;
}

RecognitionSessionManager::ReloadSnapshot RecognitionSessionManager::GetReloadSnapshot() const {
  const std::scoped_lock lock(state_mutex_);
  return ReloadSnapshot{
      .target_provider_id = target_provider_id_,
      .target_model_id = target_model_id_,
      .effective_provider_id = effective_provider_id_,
      .effective_model_id = effective_model_id_,
      .target_signature = target_backend_signature_,
      .effective_signature = effective_backend_signature_,
      .last_error = last_reload_error_,
      .reload_in_progress = reload_in_progress_,
      .has_effective_backend = effective_backend_ != nullptr,
  };
}

void RecognitionSessionManager::SetNotificationBridge(
    vinput::notify::NotificationBridge* notification_bridge) {
  const std::scoped_lock lock(state_mutex_);
  notification_bridge_ = notification_bridge;
}

void RecognitionSessionManager::SetReloadResultCallback(
    std::function<void(bool success, const std::string& message)> callback) {
  const std::scoped_lock lock(state_mutex_);
  reload_result_callback_ = std::move(callback);
}

void RecognitionSessionManager::Shutdown() {
  {
    const std::scoped_lock lock(state_mutex_);
    reload_worker_running_ = false;
    reload_requested_ = false;
  }
  reload_cv_.notify_all();
  if (reload_worker_.joinable()) {
    reload_worker_.join();
  }

  const std::scoped_lock lock(state_mutex_);
  ResetEffectiveBackendLocked();
  target_provider_id_.clear();
  target_model_id_.clear();
  target_backend_signature_.clear();
  last_reload_error_.clear();
}

bool RecognitionSessionManager::CreatePreparedBackend(
    const CoreConfig& settings, PreparedBackend* prepared, std::string* error,
    vinput::notify::NotificationBridge* notification_bridge) {
  if (prepared == nullptr) {
    if (error != nullptr) {
      *error = "Prepared backend output is not available.";
    }
    return false;
  }

  LogActiveBackend(settings);
  const auto prepare_started_at = std::chrono::steady_clock::now();
  std::string create_error;
  auto backend = std::shared_ptr<AsrBackend>(
      CreateBackend(settings, &create_error, notification_bridge).release());
  if (!backend) {
    if (error != nullptr) {
      *error = std::move(create_error);
    }
    return false;
  }

  const BackendDescriptor descriptor = backend->Describe();

  // Force backend initialization during preparation so recording does not
  // become the hidden fallback path for expensive model load. Current offline
  // backends (for example sherpa-offline) still perform their
  // heavy recognizer/model init inside CreateSession(), so warmup must keep
  // going through that path instead of only constructing the backend object.
  std::string warmup_error;
  auto warmup_session = backend->CreateSession(&warmup_error);
  if (!warmup_session) {
    if (error != nullptr) {
      *error = std::move(warmup_error);
    }
    return false;
  }
  warmup_session->Cancel();
  const auto prepare_elapsed_ms = vinput::time::MillisecondsSince(prepare_started_at);
  VLOG_DEBUG("ASR backend prepared and warmed up in {} ms", prepare_elapsed_ms);

  prepared->backend = std::move(backend);
  prepared->descriptor = descriptor;
  prepared->provider_id = descriptor.provider_id;
  prepared->model_id = ResolvePreferredLocalModel(settings);
  prepared->signature = BuildRuntimeSignature(settings);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool RecognitionSessionManager::ActivatePreparedBackend(PreparedBackend prepared,
                                                        std::string* error) {
  if (!prepared.backend) {
    if (error != nullptr) {
      *error = "Prepared backend is empty.";
    }
    return false;
  }

  {
    const std::scoped_lock lock(state_mutex_);
    effective_backend_ = std::move(prepared.backend);
    effective_descriptor_ = std::move(prepared.descriptor);
    has_effective_descriptor_ = true;
    effective_provider_id_ = std::move(prepared.provider_id);
    effective_model_id_ = std::move(prepared.model_id);
    effective_backend_signature_ = std::move(prepared.signature);
    target_provider_id_ = effective_provider_id_;
    target_model_id_ = effective_model_id_;
    target_backend_signature_ = effective_backend_signature_;
    last_reload_error_.clear();
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool RecognitionSessionManager::CreatePreparedBackendForReload(
    const CoreConfig& settings, const std::string& signature, PreparedBackend* prepared,
    std::string* error, vinput::notify::NotificationBridge* notification_bridge) {
  if (!CreatePreparedBackend(settings, prepared, error, notification_bridge)) {
    return false;
  }
  prepared->signature = signature;
  return true;
}

bool RecognitionSessionManager::CreateSessionFromEffectiveBackend(
    BackendDescriptor* descriptor, std::unique_ptr<RecognitionSession>* session,
    std::string* error) {
  if (session == nullptr) {
    if (error != nullptr) {
      *error = "Recognition session output is not available.";
    }
    return false;
  }

  std::shared_ptr<AsrBackend> backend;
  BackendDescriptor local_descriptor;
  bool have_descriptor = false;
  {
    const std::scoped_lock lock(state_mutex_);
    if (!effective_backend_) {
      if (error != nullptr) {
        if (!last_reload_error_.empty()) {
          *error = last_reload_error_;
        } else if (reload_in_progress_) {
          *error = "ASR backend is still loading.";
        } else {
          *error = "ASR backend is not ready.";
        }
      }
      return false;
    }

    if (descriptor != nullptr && has_effective_descriptor_) {
      local_descriptor = effective_descriptor_;
      have_descriptor = true;
    }
    backend = effective_backend_;
    if (!backend) {
      if (error != nullptr) {
        *error = "ASR backend is not ready.";
      }
      return false;
    }
  }

  if (descriptor != nullptr && have_descriptor) {
    *descriptor = std::move(local_descriptor);
  }

  auto backend_session = backend->CreateSession(error);
  if (!backend_session) {
    return false;
  }
  *session =
      std::make_unique<BackendKeepingSession>(std::move(backend), std::move(backend_session));
  return true;
}

void RecognitionSessionManager::EnsureReloadWorkerStarted() {
  const std::scoped_lock lock(state_mutex_);
  if (reload_worker_running_) {
    return;
  }
  reload_worker_running_ = true;
  reload_worker_ = std::thread([this]() { ReloadWorkerMain(); });
}

void RecognitionSessionManager::ReloadWorkerMain() {
  while (true) {
    CoreConfig settings;
    std::string signature;
    vinput::notify::NotificationBridge* bridge = nullptr;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      reload_cv_.wait(lock, [&]() { return reload_requested_ || !reload_worker_running_; });
      if (!reload_worker_running_) {
        break;
      }

      settings = pending_settings_;
      signature = target_backend_signature_;
      bridge = notification_bridge_;
      reload_requested_ = false;
      reload_in_progress_ = true;
    }

    PreparedBackend prepared;
    std::string error;
    const auto reload_started_at = std::chrono::steady_clock::now();
    VLOG_DEBUG("ASR backend reload worker started");
    const bool ok = CreatePreparedBackendForReload(settings, signature, &prepared, &error, bridge);
    const auto reload_elapsed_ms = vinput::time::MillisecondsSince(reload_started_at);

    if (ok) {
      {
        const std::scoped_lock lock(state_mutex_);
        effective_backend_ = std::move(prepared.backend);
        effective_descriptor_ = std::move(prepared.descriptor);
        has_effective_descriptor_ = true;
        effective_provider_id_ = std::move(prepared.provider_id);
        effective_model_id_ = std::move(prepared.model_id);
        effective_backend_signature_ = std::move(prepared.signature);
        last_reload_error_.clear();
        reload_in_progress_ = false;
      }
      VLOG_DEBUG("ASR backend reload applied asynchronously in {} ms", reload_elapsed_ms);
      NotifyReloadResult(true, {});
      continue;
    }

    {
      const std::scoped_lock lock(state_mutex_);
      last_reload_error_ = error;
      reload_in_progress_ = false;
    }
    fprintf(stderr, "vinput-daemon: async ASR backend reload failed after %lld ms: %s\n",
            static_cast<long long>(reload_elapsed_ms), error.c_str());
    NotifyReloadResult(false, error);
  }
}

void RecognitionSessionManager::ResetEffectiveBackendLocked() {
  effective_backend_.reset();
  has_effective_descriptor_ = false;
  effective_descriptor_ = {};
  effective_provider_id_.clear();
  effective_model_id_.clear();
  effective_backend_signature_.clear();
}

void RecognitionSessionManager::ApplyDisabledStateLocked() {
  pending_settings_ = {};
  target_provider_id_.clear();
  target_model_id_.clear();
  target_backend_signature_.clear();
  last_reload_error_.clear();
  reload_requested_ = false;
  reload_in_progress_ = false;
  ResetEffectiveBackendLocked();
}

void RecognitionSessionManager::NotifyReloadResult(bool success, const std::string& message) {
  std::function<void(bool, const std::string&)> callback;
  {
    const std::scoped_lock lock(state_mutex_);
    callback = reload_result_callback_;
  }
  if (callback) {
    callback(success, message);
  }
}

} // namespace vinput::daemon::asr
