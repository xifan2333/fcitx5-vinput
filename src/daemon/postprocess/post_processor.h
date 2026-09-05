#pragma once

#include <atomic>
#include <string>

#include "common/asr/recognition_result.h"
#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"

class PostProcessor {
public:
  PostProcessor();
  ~PostProcessor();

  void Shutdown();

  vinput::result::Payload Process(const std::string& raw_text,
                                  const vinput::scene::Definition& scene,
                                  const CoreConfig& settings, std::string* error_out = nullptr,
                                  const std::atomic<bool>* cancel_flag = nullptr) const;

  vinput::result::Payload ProcessCommand(const std::string& asr_text,
                                         const std::string& selected_text,
                                         const vinput::scene::Definition& command_scene,
                                         const CoreConfig& settings,
                                         std::string* error_out = nullptr,
                                         const std::atomic<bool>* cancel_flag = nullptr) const;

private:
  mutable std::atomic<bool> shutting_down_{false};
};
