#pragma once

#include "common/core_config.h"
#include "common/postprocess_scene.h"
#include "common/recognition_result.h"

class PostProcessor {
public:
  PostProcessor();
  ~PostProcessor();

  vinput::result::Payload Process(const std::string &raw_text,
                                  const vinput::scene::Definition &scene,
                                  const CoreConfig &settings) const;

  vinput::result::Payload ProcessCommand(const std::string &asr_text,
                                         const std::string &selected_text,
                                         const CoreConfig &settings) const;
};
