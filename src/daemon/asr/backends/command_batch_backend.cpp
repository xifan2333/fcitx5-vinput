#include "daemon/asr/backends/command_batch_backend.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

namespace {

std::string FormatProviderError(std::string detail, std::string_view fallback) {
  detail = std::string(vinput::str::Trim(detail));
  if (detail.empty()) {
    detail = std::string(fallback);
  }
  return detail;
}

class CommandBatchSession : public RecognitionSession {
public:
  CommandBatchSession(vinput::process::CommandSpec command, std::string provider_id)
      : command_(std::move(command)), provider_id_(std::move(provider_id)) {}

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

    if (command_result.launch_failed) {
      events_.push_back(
          {RecognitionEventKind::Error,
           {},
           FormatProviderError(std::move(command_result.stderr_text), "failed to start.")});
    } else if (command_result.timed_out) {
      events_.push_back({RecognitionEventKind::Error,
                         {},
                         FormatProviderError(std::move(command_result.stderr_text), "timed out.")});
    } else if (command_result.exit_code != 0) {
      events_.push_back({RecognitionEventKind::Error,
                         {},
                         FormatProviderError(std::move(command_result.stderr_text), "failed.")});
    } else {
      std::string text = std::string(vinput::str::Trim(command_result.stdout_text));
      if (text.empty()) {
        events_.push_back(
            {RecognitionEventKind::Error, {}, FormatProviderError({}, "returned no text.")});
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
    events_.clear();
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
  }

  std::vector<RecognitionEvent> PollEvents() override {
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

private:
  vinput::process::CommandSpec command_;
  std::string provider_id_;
  bool finished_ = false;
  std::vector<int16_t> pcm_;
  std::vector<RecognitionEvent> events_;
};

class CommandBatchBackend : public AsrBackend {
public:
  CommandBatchBackend(vinput::process::CommandSpec command, std::string provider_id)
      : command_(std::move(command)), provider_id_(std::move(provider_id)) {}

  [[nodiscard]] BackendDescriptor Describe() const override {
    BackendDescriptor descriptor;
    descriptor.provider_id = provider_id_;
    descriptor.provider_type = vinput::asr::kCommandProviderType;
    descriptor.backend_id = "command-batch";
    descriptor.capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
    return descriptor;
  }

  std::unique_ptr<RecognitionSession> CreateSession(std::string* error) override {
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<CommandBatchSession>(command_, provider_id_);
  }

private:
  vinput::process::CommandSpec command_;
  std::string provider_id_;
};

} // namespace

std::unique_ptr<AsrBackend> CreateCommandBatchBackend(const CommandAsrProvider& provider,
                                                      std::string* error) {
  if (provider.command.empty()) {
    if (error != nullptr) {
      *error = "Command ASR provider has empty command.";
    }
    return nullptr;
  }

  vinput::process::CommandSpec command;
  command.command = provider.command;
  command.args = provider.args;
  command.env = provider.env;
  command.timeout_ms = provider.timeoutMs;

  if (error != nullptr) {
    error->clear();
  }
  return std::make_unique<CommandBatchBackend>(std::move(command), provider.id);
}

} // namespace vinput::daemon::asr
