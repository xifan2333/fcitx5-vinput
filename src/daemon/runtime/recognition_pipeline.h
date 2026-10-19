#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/config/core_config_types.h"
#include "common/dbus/error_info.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/postprocess/post_processor.h"

namespace vinput::daemon::runtime {

struct RecognitionOrder {
  vinput::daemon::asr::AudioDeliveryMode audio_delivery_mode =
      vinput::daemon::asr::AudioDeliveryMode::Chunked;
  std::shared_ptr<vinput::daemon::asr::RecognitionSession> session;
  vinput::daemon::asr::BackendDescriptor backend;
  std::vector<int16_t> pcm;
  std::string recognized_text;
  std::string scene_id;
  bool is_command = false;
  std::string selected_text;
};

struct RecognitionPipelineResult {
  vinput::result::Payload payload;
  std::vector<vinput::dbus::ErrorInfo> errors;
};

class RecognitionPipeline {
public:
  explicit RecognitionPipeline(PostProcessor* post_processor);

  RecognitionPipelineResult Process(const RecognitionOrder& order, const CoreConfig& settings,
                                    const std::function<void()>& on_enter_postprocessing = {},
                                    const std::atomic<bool>* cancel_flag = nullptr) const;

private:
  PostProcessor* post_processor_ = nullptr;
};

} // namespace vinput::daemon::runtime
