#include "daemon/asr/runtime/backend_factory.h"

#include "common/asr/model_manager.h"

#include "daemon/asr/backends/command_batch_backend.h"
#include "daemon/asr/backends/command_streaming_backend.h"

#include "config.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include "daemon/asr/backends/sherpa_offline_backend.h"
#include "daemon/asr/backends/sherpa_streaming_backend.h"
#endif

namespace vinput::daemon::asr {

namespace {

bool IsStreamingCommandProvider(const CommandAsrProvider& provider) {
  static constexpr std::string_view kSuffix = ".streaming";
  return provider.id.ends_with(kSuffix);
}

std::unique_ptr<AsrBackend>
CreateLocalBackend(const CoreConfig& config, const LocalAsrProvider& provider, std::string* error) {
#if !VINPUT_ENABLE_LOCAL_ASR
  (void)config;
  (void)provider;
  if (error) {
    *error = "Local ASR support is disabled in this Lite build.";
  }
  return nullptr;
#else
  if (provider.model.empty()) {
    if (error) {
      *error = "Local ASR provider model is not configured.";
    }
    return nullptr;
  }

  ModelManager model_mgr(ResolveModelBaseDir(config).string(), provider.model);
  if (auto ensure_result = model_mgr.EnsureModels(); !ensure_result) {
    if (error) {
      *error = "Local ASR model check failed for provider '" + provider.id +
               "': " + std::move(ensure_result).error();
    }
    return nullptr;
  }

  auto model_info_result = model_mgr.GetModelInfo();
  if (!model_info_result) {
    if (error) {
      *error = "Failed to read local ASR model metadata for provider '" + provider.id +
               "': " + std::move(model_info_result).error();
    }
    return nullptr;
  }

  const std::string backend_id =
      model_info_result->backend.empty() ? "sherpa-offline" : model_info_result->backend;
  if (backend_id == "sherpa-offline") {
    return CreateSherpaOfflineBackend(config, provider, error);
  }

  if (backend_id == "sherpa-streaming") {
    return CreateSherpaStreamingBackend(config, provider, error);
  }

  if (error) {
    *error =
        "Unsupported local ASR backend '" + backend_id + "' for provider '" + provider.id + "'.";
  }
  return nullptr;
#endif
}

} // namespace

std::unique_ptr<AsrBackend> CreateBackend(const CoreConfig& config, std::string* error) {
  const AsrProvider* provider = ResolveActiveAsrProvider(config);
  if (!provider) {
    if (error) {
      *error = "Active ASR provider not found.";
    }
    return nullptr;
  }

  if (const auto* local = std::get_if<LocalAsrProvider>(provider)) {
    return CreateLocalBackend(config, *local, error);
  }
  if (const auto* command = std::get_if<CommandAsrProvider>(provider)) {
    if (IsStreamingCommandProvider(*command)) {
      return CreateCommandStreamingBackend(*command, error);
    }
    return CreateCommandBatchBackend(*command, error);
  }

  if (error) {
    *error = "Unsupported ASR provider type: " + std::string(AsrProviderType(*provider));
  }
  return nullptr;
}

bool DescribeActiveBackend(const CoreConfig& config, BackendDescriptor* descriptor,
                           std::string* error) {
  auto backend = CreateBackend(config, error);
  if (!backend) {
    return false;
  }
  if (descriptor) {
    *descriptor = backend->Describe();
  }
  if (error) {
    error->clear();
  }
  return true;
}

} // namespace vinput::daemon::asr
