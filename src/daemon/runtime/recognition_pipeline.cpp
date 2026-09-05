#include "daemon/runtime/recognition_pipeline.h"

#include "common/dbus/error_info.h"
#include "common/utils/debug_log.h"

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
  vinput::debug::Log("recognized text command=%s scene=%s: %s\n", is_command ? "true" : "false",
                     scene_id.empty() ? "(default)" : std::string(scene_id).c_str(),
                     QuoteRecognitionText(text).c_str());
}

} // namespace

namespace vinput::daemon::runtime {

RecognitionPipeline::RecognitionPipeline(PostProcessor* post_processor)
    : post_processor_(post_processor) {}

RecognitionPipelineResult
RecognitionPipeline::Process(const RecognitionOrder& order, const CoreConfig& settings,
                             const std::function<void()>& on_enter_postprocessing) const {
  RecognitionPipelineResult output;
  if (!post_processor_) {
    output.errors.push_back(vinput::dbus::MakeRawError("Recognition pipeline is not initialized."));
    return output;
  }

  if (order.recognized_text.empty()) {
    return output;
  }

  if (vinput::debug::Enabled()) {
    LogRecognizedText(order.recognized_text, order.is_command, order.scene_id);
  }

  vinput::scene::Config scene_config;
  scene_config.activeSceneId = settings.scenes.activeScene;
  scene_config.scenes = settings.scenes.definitions;

  if (order.is_command) {
    const auto* cmd_scene = FindCommandScene(settings);
    if (cmd_scene != nullptr && cmd_scene->llm_max_candidates > 0 &&
        !cmd_scene->provider_id.empty() && on_enter_postprocessing) {
      on_enter_postprocessing();
    }

    vinput::scene::Definition fallback_cmd;
    fallback_cmd.id = std::string(vinput::scene::kCommandSceneId);
    fallback_cmd.builtin = true;
    const auto& command_scene = cmd_scene ? *cmd_scene : fallback_cmd;
    std::string llm_error;
    output.payload = post_processor_->ProcessCommand(order.recognized_text, order.selected_text,
                                                     command_scene, settings, &llm_error);
    if (!llm_error.empty()) {
      output.errors.push_back(vinput::dbus::ClassifyErrorText(llm_error));
    }
    return output;
  }

  const auto& scene = vinput::scene::Resolve(scene_config, order.scene_id);
  if (scene.llm_max_candidates > 0 && !scene.provider_id.empty() && on_enter_postprocessing) {
    on_enter_postprocessing();
  }

  std::string llm_error;
  output.payload = post_processor_->Process(order.recognized_text, scene, settings, &llm_error);
  if (!llm_error.empty()) {
    output.errors.push_back(vinput::dbus::ClassifyErrorText(llm_error));
  }
  return output;
}

} // namespace vinput::daemon::runtime
