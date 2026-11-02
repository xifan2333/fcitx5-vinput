#include "daemon/runtime/recognition_pipeline.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/debug_log.h"

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

const vinput::scene::Definition* ResolveDefinition(const CoreConfig& settings,
                                                   std::string_view scene_id) {
  // 1. Search requested scene_id
  for (const auto& def : settings.scenes.definitions) {
    if (def.id == scene_id) {
      return &def;
    }
  }
  // 2. Search active scene if not empty
  if (!settings.scenes.activeScene.empty()) {
    for (const auto& def : settings.scenes.definitions) {
      if (def.id == settings.scenes.activeScene) {
        return &def;
      }
    }
  }
  return nullptr;
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

  if (request.is_command) {
    const auto* cmd_scene = FindCommandScene(settings);
    vinput::scene::Definition fallback_cmd;
    fallback_cmd.id = std::string(vinput::scene::kCommandSceneId);
    fallback_cmd.builtin = true;
    const auto& command_scene = (cmd_scene != nullptr) ? *cmd_scene : fallback_cmd;

    if (command_scene.llm_max_candidates > 0 && !command_scene.provider_id.empty() &&
        on_enter_postprocessing) {
      on_enter_postprocessing();
    }

    std::string llm_error;
    output.payload = post_processor_->ProcessCommand(
        request.text, request.selected_text, command_scene, settings, &llm_error, cancel_flag);
    if (!llm_error.empty()) {
      output.errors.push_back(std::move(llm_error));
    }
    return output;
  }

  const auto* found_scene = ResolveDefinition(settings, request.scene_id);
  static const vinput::scene::Definition empty_scene;
  const auto& scene = (found_scene != nullptr) ? *found_scene : empty_scene;

  if (scene.llm_max_candidates > 0 && !scene.provider_id.empty() && on_enter_postprocessing) {
    on_enter_postprocessing();
  }

  std::string llm_error;
  output.payload = post_processor_->Process(request.text, scene, settings, &llm_error, cancel_flag);
  if (!llm_error.empty()) {
    output.errors.push_back(std::move(llm_error));
  }
  return output;
}

} // namespace vinput::daemon::runtime
