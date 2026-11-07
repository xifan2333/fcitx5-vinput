#pragma once

#include <atomic>
#include <string>

#include "common/asr/recognition_result.h"
#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"

namespace vinput::notify {
class NotificationBridge;
}

class PostProcessor {
public:
  explicit PostProcessor(vinput::notify::NotificationBridge* notification_bridge = nullptr);
  ~PostProcessor() = default;

  PostProcessor(const PostProcessor&) = delete;
  PostProcessor& operator=(const PostProcessor&) = delete;
  PostProcessor(PostProcessor&&) = delete;
  PostProcessor& operator=(PostProcessor&&) = delete;

  void SetNotificationBridge(vinput::notify::NotificationBridge* notification_bridge);
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
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
};
