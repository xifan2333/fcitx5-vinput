#pragma once

#include <memory>
#include <string>

#include "common/config/core_config_types.h"

#include "daemon/asr/local_asr_model_loader.h"
#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

std::unique_ptr<AsrBackend> CreateSherpaStreamingBackend(const LocalAsrProvider& provider,
                                                         LocalAsrModelContext context,
                                                         std::string* error);

} // namespace vinput::daemon::asr
