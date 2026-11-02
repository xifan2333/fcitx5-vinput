#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/config/core_config_types.h"

class PostProcessor;

namespace vinput::daemon::runtime {

struct PostprocessRequest {
  std::string text;
  std::string scene_id;
  bool is_command = false;
  std::string selected_text;
};

struct RecognitionPipelineResult {
  vinput::result::Payload payload;
  std::vector<std::string> errors;
};

class RecognitionPipeline {
public:
  explicit RecognitionPipeline(PostProcessor* post_processor);

  [[nodiscard]] RecognitionPipelineResult
  Process(const PostprocessRequest& request, const CoreConfig& settings,
          const std::function<void()>& on_enter_postprocessing = {},
          const std::atomic<bool>* cancel_flag = nullptr) const;

private:
  PostProcessor* post_processor_ = nullptr;
};

} // namespace vinput::daemon::runtime
