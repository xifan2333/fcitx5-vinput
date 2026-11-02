#include "daemon/runtime/recognition_pipeline.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/debug_log.h"
#include "common/utils/string_utils.h"

#include "daemon/postprocess/post_processor.h"

namespace {

constexpr std::size_t kMaxLoggedRecognitionBytes = 2048;

std::string QuoteRecognitionText(std::string_view text) {
  if (text.size() > kMaxLoggedRecognitionBytes) {
    std::string truncated(text.substr(0, kMaxLoggedRecognitionBytes));
    truncated += "...(truncated)";
    return truncated;
  }
  return std::string(text);
}

void LogRecognizedText(const std::string& text, bool is_command, std::string_view scene_id) {
  VLOG_DEBUG("recognized text command={} scene={}: {}", is_command ? "true" : "false",
             scene_id.empty() ? "(default)" : scene_id, QuoteRecognitionText(text));
}

} // namespace

namespace vinput::daemon::runtime {

RecognitionPipeline::RecognitionPipeline(PostProcessor* post_processor)
    : post_processor_(post_processor) {}

RecognitionPipelineResult
RecognitionPipeline::Process(const PostprocessRequest& request, const CoreConfig& settings,
                             const std::function<void()>& on_enter_postprocessing,
                             const std::atomic<bool>* cancel_flag) const {
  RecognitionPipelineResult output;
  if (post_processor_ == nullptr) {
    output.errors.emplace_back("Recognition pipeline is not initialized.");
    return output;
  }

  if (request.text.empty()) {
    return output;
  }

  LogRecognizedText(request.text, request.is_command, request.scene_id);

  auto invoke_processor = [&](auto&& proc_fn) {
    std::string llm_error;
    output.payload = proc_fn(&llm_error);
    if (!llm_error.empty()) {
      output.errors.push_back(std::move(llm_error));
    }
  };

  if (request.is_command) {
    const auto* cmd_scene = FindCommandScene(settings);
    vinput::scene::Definition fallback_cmd;
    fallback_cmd.id = std::string(vinput::scene::kCommandSceneId);
    fallback_cmd.builtin = true;
    const auto& command_scene = (cmd_scene != nullptr) ? *cmd_scene : fallback_cmd;

    if (command_scene.llm_max_candidates > 0 && !command_scene.provider_id.empty()) {
      if (on_enter_postprocessing) {
        on_enter_postprocessing();
      }
      invoke_processor([&](std::string* err) {
        return post_processor_->ProcessCommand(request.text, request.selected_text, command_scene,
                                               settings, err, cancel_flag);
      });
    } else {
      output.payload.commitText = request.text;
    }
    return output;
  }

  const std::string_view target_scene_id = request.scene_id.empty()
                                               ? std::string_view(settings.scenes.activeScene)
                                               : std::string_view(request.scene_id);

  const vinput::scene::Definition* target_scene = nullptr;
  for (const auto& def : settings.scenes.definitions) {
    if (def.id == target_scene_id) {
      target_scene = &def;
      break;
    }
  }

  if (target_scene == nullptr) {
    output.payload.commitText = request.text;
    output.errors.push_back(
        vinput::str::FmtStr(_("Scene '%s' not found."), std::string(target_scene_id).c_str()));
    return output;
  }

  if (target_scene->llm_max_candidates > 0 && !target_scene->provider_id.empty()) {
    if (on_enter_postprocessing) {
      on_enter_postprocessing();
    }
    invoke_processor([&](std::string* err) {
      return post_processor_->Process(request.text, *target_scene, settings, err, cancel_flag);
    });
  } else {
    output.payload.commitText = request.text;
  }
  return output;
}

} // namespace vinput::daemon::runtime
