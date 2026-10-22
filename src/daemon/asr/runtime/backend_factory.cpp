#include "daemon/asr/runtime/backend_factory.h"

#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"

#include "daemon/asr/backends/command_batch_backend.h"
#include "daemon/asr/backends/command_streaming_backend.h"
#include "daemon/asr/runtime/recognition_contract.h"

#include "config.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include <utility>

#include "daemon/asr/backends/sherpa_offline_backend.h"
#include "daemon/asr/backends/sherpa_streaming_backend.h"
#include "daemon/asr/local_asr_model_loader.h"
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
  if (error != nullptr) {
    *error = "Local ASR support is disabled in this Lite build.";
  }
  return nullptr;
#else
  auto context_result = LoadLocalAsrModelContext(config, provider);
  if (!context_result) {
    if (error != nullptr) {
      *error = std::move(context_result).error();
    }
    return nullptr;
  }

  const std::string backend_id =
      context_result->info.backend.empty() ? "sherpa-offline" : context_result->info.backend;
  if (backend_id == "sherpa-offline") {
    return CreateSherpaOfflineBackend(config, provider, std::move(*context_result), error);
  }

  if (backend_id == "sherpa-streaming") {
    return CreateSherpaStreamingBackend(provider, std::move(*context_result), error);
  }

  if (error != nullptr) {
    *error =
        "Unsupported local ASR backend '" + backend_id + "' for provider '" + provider.id + "'.";
  }
  return nullptr;
#endif
}

} // namespace

std::unique_ptr<AsrBackend> CreateBackend(const CoreConfig& config, std::string* error) {
  const AsrProvider* provider = ResolveActiveAsrProvider(config);
  if (provider == nullptr) {
    if (error != nullptr) {
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

  if (error != nullptr) {
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
  if (descriptor != nullptr) {
    *descriptor = backend->Describe();
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace vinput::daemon::asr
