#include "daemon/asr/backends/command_batch_backend.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/debug_log.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

namespace {

constexpr std::size_t kMaxBatchNotifications = 10;

vinput::process::CommandSpec BuildCommandSpec(const CommandAsrProvider& provider) {
  vinput::process::CommandSpec spec;
  spec.command = provider.command;
  spec.args = provider.args;
  spec.env = provider.env;
  spec.timeout_ms = provider.timeoutMs;
  return spec;
}

class CommandBatchSession : public RecognitionSession {
public:
  CommandBatchSession(const CommandAsrProvider& provider,
                      vinput::notify::NotificationBridge* notification_bridge)
      : command_(BuildCommandSpec(provider)), provider_id_(provider.id),
        notification_bridge_(notification_bridge) {}

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    if (finished_) {
      if (error != nullptr) {
        *error = "Recognition session already finished.";
      }
      return false;
    }
    pcm_.insert(pcm_.end(), pcm.begin(), pcm.end());
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool Finish(std::string* error) override {
    if (finished_) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    finished_ = true;
    const auto audio = std::as_bytes(std::span<const int16_t>(pcm_));
    auto command_result = vinput::process::Run(command_, audio);

    // The provider owns the wording of its diagnostics: a one-shot run leaves a complete stderr
    // capture behind, so it is handed to the bridge in a single pass (ADR 0001) instead of being
    // folded into this backend's own error text.
    DeliverStderr(command_result.stderr_text);

    if (command_result.launch_failed) {
      events_.push_back(
          {RecognitionEventKind::Error, {}, _("Command ASR provider failed to start.")});
    } else if (command_result.timed_out) {
      events_.push_back({RecognitionEventKind::Error, {}, _("Command ASR provider timed out.")});
    } else if (command_result.exit_code != 0) {
      VLOG_WARN("command ASR provider '{}' exited with code {}", provider_id_,
                command_result.exit_code);
      events_.push_back(
          {RecognitionEventKind::Error, {}, _("Command ASR provider exited abnormally.")});
    } else {
      std::string text = std::string(vinput::str::Trim(command_result.stdout_text));
      if (text.empty()) {
        events_.push_back(
            {RecognitionEventKind::Error, {}, _("Command ASR provider returned no text.")});
      } else {
        events_.push_back({RecognitionEventKind::FinalText, std::move(text), {}});
      }
    }

    events_.push_back({RecognitionEventKind::Completed, {}, {}});
    if (error != nullptr) {
      if (!events_.empty() && events_.front().kind == RecognitionEventKind::Error) {
        *error = events_.front().error;
      } else {
        error->clear();
      }
    }
    return error == nullptr || error->empty();
  }

  void Cancel() override {
    finished_ = true;
    pcm_.clear();
    events_ = {{RecognitionEventKind::Completed, {}, {}}};
  }

  std::vector<RecognitionEvent> PollEvents() override {
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

private:
  void DeliverStderr(std::string stderr_text) const {
    if (notification_bridge_ == nullptr || stderr_text.empty()) {
      return;
    }
    std::size_t delivered = 0;
    vinput::process::DrainLinesFromBuffer(
        stderr_text,
        [this, &delivered](std::string_view line) -> bool {
          if (delivered >= kMaxBatchNotifications) {
            return false;
          }
          notification_bridge_->DeliverLine(provider_id_, line);
          ++delivered;
          return true;
        },
        /*flush_tail=*/true);
  }

  vinput::process::CommandSpec command_;
  std::string provider_id_;
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
  bool finished_ = false;
  std::vector<int16_t> pcm_;
  std::vector<RecognitionEvent> events_;
};

class CommandBatchBackend : public AsrBackend {
public:
  CommandBatchBackend(CommandAsrProvider provider,
                      vinput::notify::NotificationBridge* notification_bridge)
      : provider_(std::move(provider)), notification_bridge_(notification_bridge) {}

  [[nodiscard]] BackendDescriptor Describe() const override {
    BackendDescriptor descriptor;
    descriptor.provider_id = provider_.id;
    descriptor.provider_type = vinput::asr::kCommandProviderType;
    descriptor.backend_id = "command-batch";
    descriptor.capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
    return descriptor;
  }

  std::unique_ptr<RecognitionSession> CreateSession(std::string* error) override {
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<CommandBatchSession>(provider_, notification_bridge_);
  }

private:
  CommandAsrProvider provider_;
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
};

} // namespace

std::unique_ptr<AsrBackend>
CreateCommandBatchBackend(const CommandAsrProvider& provider, std::string* error,
                          vinput::notify::NotificationBridge* notification_bridge) {
  if (provider.command.empty()) {
    if (error != nullptr) {
      *error = "Command ASR provider has empty command.";
    }
    return nullptr;
  }

  if (error != nullptr) {
    error->clear();
  }
  return std::make_unique<CommandBatchBackend>(provider, notification_bridge);
}

} // namespace vinput::daemon::asr
