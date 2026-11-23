#pragma once

#include <cstdint>
#include <string>

#include "common/dbus/dbus_interface.h"

namespace vinput::dbus {

enum class RequestedAsrBackendStatus : std::uint8_t {
  kUnknown,
  kConfigSaved,
  kReloadInProgress,
  kApplied,
  kFailedStillUsingPrevious,
  kFailedNoUsableBackend,
};

inline bool MatchesRequestedAsrBackend(const AsrBackendState& state, const std::string& provider_id,
                                       const std::string& model_id, bool effective) {
  const std::string& state_provider =
      effective ? state.effective_provider_id : state.target_provider_id;
  const std::string& state_model = effective ? state.effective_model_id : state.target_model_id;
  return state_provider == provider_id && state_model == model_id;
}

inline RequestedAsrBackendStatus ClassifyRequestedAsrBackend(const AsrBackendState& state,
                                                             const std::string& provider_id,
                                                             const std::string& model_id) {
  const bool target_matches = MatchesRequestedAsrBackend(state, provider_id, model_id, false);
  const bool effective_matches = MatchesRequestedAsrBackend(state, provider_id, model_id, true);

  if (state.reload_in_progress && target_matches) {
    return RequestedAsrBackendStatus::kReloadInProgress;
  }
  if (!state.last_error.empty() && target_matches && !effective_matches) {
    return state.has_effective_backend ? RequestedAsrBackendStatus::kFailedStillUsingPrevious
                                       : RequestedAsrBackendStatus::kFailedNoUsableBackend;
  }
  if (effective_matches) {
    return RequestedAsrBackendStatus::kApplied;
  }
  if (target_matches) {
    return RequestedAsrBackendStatus::kConfigSaved;
  }
  return RequestedAsrBackendStatus::kUnknown;
}

} // namespace vinput::dbus
