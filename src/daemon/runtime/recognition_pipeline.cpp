#include "daemon/runtime/recognition_pipeline.h"

#include <atomic>
#include <cstddef>
#include <fmt/printf.h>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/i18n.h"
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

const vinput::scene::Definition* FindSceneDefinition(const CoreConfig& settings,
                                                     std::string_view target_scene_id) {
  for (const auto& def : settings.scenes.definitions) {
    if (def.id == target_scene_id) {
      return &def;
    }
  }
  return nullptr;
}

vinput::scene::Definition ResolveCommandScene(const CoreConfig& settings) {
  const auto* cmd_scene = CoreConfigView(settings).CommandScene();
  if (cmd_scene != nullptr) {
    return *cmd_scene;
  }
  vinput::scene::Definition fallback_cmd;
  fallback_cmd.id = std::string(vinput::scene::kCommandSceneId);
  fallback_cmd.builtin = true;
  return fallback_cmd;
}

template <typename Fn>
RecognitionPipelineResult
InvokePostProcessor(const PostprocessRequest& request, const vinput::scene::Definition& scene,
                    const std::function<void()>& on_enter_postprocessing, Fn&& run_proc) {
  RecognitionPipelineResult output;
  if (scene.llm_max_candidates <= 0 || scene.provider_id.empty()) {
    output.payload.commitText = request.text;
    return output;
  }

  if (on_enter_postprocessing) {
    on_enter_postprocessing();
  }

  std::string llm_error;
  output.payload = run_proc(&llm_error);
  if (!llm_error.empty()) {
    output.errors.push_back(std::move(llm_error));
  }
  return output;
}

} // namespace

namespace vinput::daemon::runtime {

RecognitionPipeline::RecognitionPipeline(PostProcessor* post_processor)
    : post_processor_(post_processor) {}

RecognitionPipelineResult
RecognitionPipeline::ProcessCommand(const PostprocessRequest& request, const CoreConfig& settings,
                                    const std::function<void()>& on_enter_postprocessing,
                                    const std::atomic<bool>* cancel_flag) const {
  const auto command_scene = ResolveCommandScene(settings);
  return InvokePostProcessor(
      request, command_scene, on_enter_postprocessing, [&](std::string* err) {
        return post_processor_->ProcessCommand(request.text, request.selected_text, command_scene,
                                               settings, err, cancel_flag);
      });
}

RecognitionPipelineResult
RecognitionPipeline::ProcessDictation(const PostprocessRequest& request, const CoreConfig& settings,
                                      const std::function<void()>& on_enter_postprocessing,
                                      const std::atomic<bool>* cancel_flag) const {
  const std::string_view target_scene_id = request.scene_id.empty()
                                               ? std::string_view(settings.scenes.activeScene)
                                               : std::string_view(request.scene_id);

  const auto* target_scene = FindSceneDefinition(settings, target_scene_id);
  if (target_scene == nullptr) {
    RecognitionPipelineResult output;
    output.payload.commitText = request.text;
    output.errors.push_back(fmt::sprintf(_("Scene '%s' not found."), target_scene_id));
    return output;
  }

  return InvokePostProcessor(
      request, *target_scene, on_enter_postprocessing, [&](std::string* err) {
        return post_processor_->Process(request.text, *target_scene, settings, err, cancel_flag);
      });
}

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
    return ProcessCommand(request, settings, on_enter_postprocessing, cancel_flag);
  }
  return ProcessDictation(request, settings, on_enter_postprocessing, cancel_flag);
}

} // namespace vinput::daemon::runtime
