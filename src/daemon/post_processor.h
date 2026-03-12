#pragma once

#include "common/core_config.h"
#include "common/postprocess_scene.h"
#include "common/recognition_result.h"

#include <string>

class PostProcessor {
public:
  PostProcessor();
  ~PostProcessor();

  vinput::result::Payload Process(const std::string &raw_text,
                                  const vinput::scene::Definition &scene,
                                  const CoreConfig &settings,
                                  std::string *error_out = nullptr) const;

  vinput::result::Payload ProcessCommand(const std::string &asr_text,
                                         const std::string &selected_text,
                                         const CoreConfig &settings,
                                         std::string *error_out = nullptr) const;
};
