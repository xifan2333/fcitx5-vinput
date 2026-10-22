#include "daemon/asr/local_asr_model_loader.h"

#include <string>
#include <utility>

#include "common/asr/model_manager.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/utils/expected.h"
#include "common/utils/string_utils.h"

namespace vinput::daemon::asr {

vinput::Expected<LocalAsrModelContext> LoadLocalAsrModelContext(const CoreConfig& config,
                                                                const LocalAsrProvider& provider) {
  if (provider.model.empty()) {
    return vinput::UnexpectedError(vinput::str::FmtStr(
        _("Local ASR model configuration is missing for provider '%s'."), provider.id));
  }

  const ModelManager model_mgr(ResolveModelBaseDir(config).string(), provider.model);
  if (auto ensure_result = model_mgr.EnsureModels(); !ensure_result) {
    return vinput::UnexpectedError("Local ASR model check failed for provider '" + provider.id +
                                   "': " + std::move(ensure_result).error());
  }

  auto model_info = model_mgr.GetModelInfo();
  if (!model_info) {
    return vinput::UnexpectedError("Failed to read local ASR model metadata for provider '" +
                                   provider.id + "': " + std::move(model_info).error());
  }

  LocalAsrModelContext context;
  context.asr_config.language = model_info->RuntimeLanguageHint();
  context.asr_config.hotwords_file = provider.hotwordsFile;
  context.info = std::move(*model_info);
  return context;
}

} // namespace vinput::daemon::asr
