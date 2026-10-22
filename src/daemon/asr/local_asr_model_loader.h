#pragma once

#include "common/asr/model_manager.h"
#include "common/config/core_config_types.h"
#include "common/utils/expected.h"

#include "daemon/asr/asr_config.h"

namespace vinput::daemon::asr {

// Parsed model metadata plus an AsrConfig pre-filled with the fields shared by
// every local sherpa-onnx backend (language hint and hotwords file). Backend
// factories only need to layer their VAD policy on top of asr_config.
struct LocalAsrModelContext {
  ModelInfo info;
  AsrConfig asr_config;
};

// Loads and validates the local model bound to a LocalAsrProvider, returning
// the parsed context on success or a diagnostic on failure.
[[nodiscard]] vinput::Expected<LocalAsrModelContext>
LoadLocalAsrModelContext(const CoreConfig& config, const LocalAsrProvider& provider);

} // namespace vinput::daemon::asr
