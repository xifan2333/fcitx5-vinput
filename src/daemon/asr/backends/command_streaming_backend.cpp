#include "daemon/asr/backends/command_streaming_backend.h"

#include <atomic>
#include <base64.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <glaze/core/common.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/debug_log.h"
#include "common/utils/process_session.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPollSliceMs = 50;
constexpr int kAwaitFinalTimeoutMs = 10000;
constexpr int kFinalGraceMs = 200;

bool HasTimedOut(Clock::time_point deadline) {
  return Clock::now() >= deadline;
}

int RemainingMs(Clock::time_point deadline, int fallback_ms) {
  return vinput::time::RemainingTimeoutMs(deadline, fallback_ms, Clock::now());
}

std::string MakeSessionSourceId(std::string_view provider_id) {
  static std::atomic<std::uint64_t> s_session_counter{0};
  return std::format("{}-{}", provider_id, ++s_session_counter);
}

class CommandStreamingSession : public RecognitionSession {
public:
  CommandStreamingSession(CommandAsrProvider provider, std::string provider_id,
                          vinput::notify::NotificationBridge* notification_bridge = nullptr)
      : provider_(std::move(provider)), provider_id_(std::move(provider_id)),
        notification_bridge_(notification_bridge), source_id_(MakeSessionSourceId(provider_id_)) {}

  CommandStreamingSession(const CommandStreamingSession&) = delete;
  CommandStreamingSession& operator=(const CommandStreamingSession&) = delete;
  CommandStreamingSession(CommandStreamingSession&&) = delete;
  CommandStreamingSession& operator=(CommandStreamingSession&&) = delete;

  ~CommandStreamingSession() override { Terminate(); }

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    if (finished_) {
      if (error != nullptr) {
        *error = "Recognition session already finished.";
      }
      return false;
    }

    if (!EnsureStarted(error)) {
      return false;
    }

    if (!FlushPendingChunk(false, error)) {
      return false;
    }

    if (!pcm.empty()) {
      const auto byte_span = std::as_bytes(pcm);
      pending_chunk_.assign(byte_span.begin(), byte_span.end());
    }

    if (!DrainEvents(std::chrono::milliseconds(0), error)) {
      return false;
    }

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

    if (!EnsureStarted(error)) {
      return false;
    }

    finished_ = true;
    if (!FlushPendingChunk(true, error)) {
      QueueError(error != nullptr && !error->empty() ? *error : "failed to flush audio chunk.");
      QueueCompleted();
      Terminate();
      return false;
    }
    if (!SendControlEvent("finish", error)) {
      QueueError(error != nullptr && !error->empty() ? *error : "failed to send finish event.");
      QueueCompleted();
      Terminate();
      return false;
    }

    if (session_.has_value()) {
      session_->CloseStdin();
    }
    deadline_ = Clock::now() + std::chrono::milliseconds(kAwaitFinalTimeoutMs);

    while (!saw_final_text_ && !completed_ && !child_exited_) {
      if (!DrainEvents(std::chrono::milliseconds(kPollSliceMs), error)) {
        QueueError(error != nullptr && !error->empty() ? *error : "failed to poll provider.");
        QueueCompleted();
        Terminate();
        return false;
      }
      if (HasTimedOut(deadline_)) {
        QueueError("Streaming ASR provider timed out.");
        QueueCompleted();
        Terminate();
        if (error != nullptr) {
          *error = events_.back().error;
        }
        return false;
      }
    }

    bool grace_pump_failed = false;
    if (saw_final_text_) {
      const Clock::time_point grace_deadline =
          Clock::now() + std::chrono::milliseconds(kFinalGraceMs);
      while (!HasTimedOut(grace_deadline) && !completed_ && !child_exited_) {
        if (!DrainEvents(std::chrono::milliseconds(RemainingMs(grace_deadline, kPollSliceMs)),
                         error)) {
          grace_pump_failed = true;
          break;
        }
      }
    }

    DrainEvents(std::chrono::milliseconds(0), nullptr);

    if (!saw_final_text_ && !saw_error_ && !cancelled_ && !killed_by_us_) {
      QueueError("Streaming ASR provider returned no text.");
    }
    QueueCompleted();
    Terminate();

    if (cancelled_ || killed_by_us_ || interrupted_.load(std::memory_order_acquire)) {
      if (error != nullptr) {
        *error = "Recognition session was interrupted or cancelled.";
      }
      return false;
    }

    if (error != nullptr) {
      if (saw_error_ && !events_.empty()) {
        for (const auto& event : events_) {
          if (event.kind == RecognitionEventKind::Error) {
            *error = event.error;
            return false;
          }
        }
      }
      if (grace_pump_failed) {
        return false;
      }
      error->clear();
    }
    return !saw_error_ && !grace_pump_failed;
  }

  void Cancel() override {
    if (!cancelled_) {
      cancelled_ = true;
      if (started_) {
        SendControlEvent("cancel", nullptr);
      }
    }
    QueueCompleted();
    Terminate();
  }

  void Interrupt() noexcept override {
    interrupted_.store(true, std::memory_order_release);
    const std::scoped_lock lock(session_mutex_);
    if (session_.has_value()) {
      session_->Cancel();
    }
  }

  std::vector<RecognitionEvent> PollEvents() override {
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

private:
  bool EnsureStarted(std::string* error) {
    if (cancelled_ || interrupted_.load(std::memory_order_acquire)) {
      if (error != nullptr) {
        *error = "Recognition session already cancelled.";
      }
      return false;
    }
    if (started_) {
      return true;
    }
    if (provider_.command.empty()) {
      if (error != nullptr) {
        *error = "Command ASR provider has empty command.";
      }
      return false;
    }

    vinput::process::CommandSpec spec;
    spec.command = provider_.command;
    spec.args = provider_.args;
    spec.env = provider_.env;
    spec.timeout_ms = provider_.timeoutMs;

    auto session_res = vinput::process::ProcessSession::Start(spec);
    if (!session_res) {
      if (error != nullptr) {
        *error = session_res.error();
      }
      QueueError(error != nullptr ? *error : "failed to start.");
      QueueCompleted();
      return false;
    }

    {
      const std::scoped_lock lock(session_mutex_);
      if (interrupted_.load(std::memory_order_acquire)) {
        session_res->Cancel();
        return false;
      }
      session_.emplace(std::move(*session_res));
    }
    started_ = true;
    return true;
  }

  bool FlushPendingChunk(bool commit, std::string* error) {
    if (pending_chunk_.empty()) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    const glz::json_t payload = {
        {"type", "audio"},
        {"audio_base64",
         base64::encode_into<std::string>(pending_chunk_.begin(), pending_chunk_.end())},
        {"commit", commit},
    };
    pending_chunk_.clear();
    return SendJsonLine(payload, error);
  }

  bool SendControlEvent(std::string_view type, std::string* error) {
    const glz::json_t payload = {
        {"type", std::string(type)},
    };
    return SendJsonLine(payload, error);
  }

  bool SendJsonLine(const glz::json_t& payload, std::string* error) {
    if (!session_.has_value()) {
      if (error != nullptr) {
        *error = "provider session is not started.";
      }
      return false;
    }

    std::string line = vinput::json::ToText(payload);
    line.push_back('\n');
    const auto bytes = std::as_bytes(std::span(line));
    if (!session_->PushStdin(bytes)) {
      DrainEvents(std::chrono::milliseconds(0), nullptr);
      if (error != nullptr) {
        *error = "Provider closed stdin or input buffer is full.";
      }
      return false;
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool DrainEvents(std::chrono::milliseconds wait, std::string* error) {
    if (!session_.has_value()) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    vinput::process::ChildEvent event;
    bool first = true;
    while (session_->NextEvent(event, first ? wait : std::chrono::milliseconds(0))) {
      first = false;
      switch (event.kind) {
      case vinput::process::ChildEventKind::StdoutChunk: {
        stdout_buffer_.reserve(stdout_buffer_.size() + event.data.size());
        for (const auto b : event.data) {
          stdout_buffer_.push_back(static_cast<char>(b));
        }
        ConsumeStdoutLines(error);
        if (error != nullptr && !error->empty()) {
          return false;
        }
        break;
      }
      case vinput::process::ChildEventKind::StderrChunk: {
        if (notification_bridge_ != nullptr) {
          notification_bridge_->FeedStderr(source_id_, event.data);
        }
        break;
      }
      case vinput::process::ChildEventKind::Cancelled: {
        killed_by_us_ = true;
        break;
      }
      case vinput::process::ChildEventKind::Exited: {
        child_exited_ = true;
        exit_code_ = event.exit_code;
        killed_by_us_ = event.killed_by_us;
        if (notification_bridge_ != nullptr) {
          notification_bridge_->Flush(source_id_);
        }
        if (killed_by_us_) {
          VLOG_DEBUG("streaming ASR provider '{}' terminated by session cancellation",
                     provider_id_);
        } else if (exit_code_ != 0 && !saw_error_ && !saw_final_text_) {
          VLOG_WARN("streaming ASR provider '{}' exited abnormally with code {}", provider_id_,
                    exit_code_);
          QueueError("Streaming ASR provider exited abnormally.");
        }
        break;
      }
      }
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void ConsumeStdoutLines(std::string* error) {
    vinput::process::DrainLinesFromBuffer(stdout_buffer_, [&](std::string_view raw_line) {
      const std::string line(vinput::str::Trim(raw_line));
      if (line.empty()) {
        return true;
      }
      return HandleOutputLine(line, error);
    });
  }

  bool HandleOutputLine(const std::string& line, std::string* error) {
    glz::json_t payload;
    if (const auto ec = glz::read_json(payload, line); bool(ec)) {
      if (error != nullptr) {
        *error = std::string("invalid streaming provider JSON: ") + glz::format_error(ec, line);
      }
      return false;
    }

    const std::string type = vinput::json::String(payload, "type");
    if (type == "session_started" || type.empty()) {
      return true;
    }
    if (type == "partial") {
      const std::string text =
          std::string(vinput::str::Trim(vinput::json::String(payload, "text")));
      if (!text.empty() && text != last_partial_text_) {
        last_partial_text_ = text;
        events_.push_back({RecognitionEventKind::PartialText, text, {}});
      }
      return true;
    }
    if (type == "final" || type == "final_timestamps") {
      const std::string text =
          std::string(vinput::str::Trim(vinput::json::String(payload, "text")));
      if (!text.empty()) {
        saw_final_text_ = true;
        last_partial_text_.clear();
        events_.push_back({RecognitionEventKind::FinalText, text, {}});
      }
      return true;
    }
    if (type == "error") {
      QueueError(std::string(vinput::str::Trim(vinput::json::String(payload, "message"))));
      return true;
    }
    if (type == "closed") {
      completed_ = true;
      return true;
    }
    return true;
  }

  void QueueError(std::string_view error_text) {
    const std::string_view trimmed = vinput::str::Trim(error_text);
    saw_error_ = true;
    events_.push_back(
        {RecognitionEventKind::Error, {}, trimmed.empty() ? "failed." : std::string(trimmed)});
  }

  void QueueCompleted() {
    if (completed_emitted_) {
      return;
    }
    completed_emitted_ = true;
    completed_ = true;
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
  }

  void Terminate() {
    if (terminated_) {
      return;
    }
    terminated_ = true;

    if (notification_bridge_ != nullptr) {
      notification_bridge_->Flush(source_id_);
      notification_bridge_->RetireSource(source_id_);
    }

    std::optional<vinput::process::ProcessSession> to_destroy;
    {
      const std::scoped_lock lock(session_mutex_);
      if (session_.has_value()) {
        session_->Cancel();
        to_destroy.emplace(std::move(*session_));
        session_.reset();
      }
    }
    to_destroy.reset();
  }

  CommandAsrProvider provider_;
  std::string provider_id_;
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
  std::string source_id_;
  Clock::time_point deadline_ = Clock::time_point::max();
  mutable std::mutex session_mutex_;
  std::optional<vinput::process::ProcessSession> session_;
  std::atomic<bool> interrupted_{false};
  bool started_ = false;
  bool finished_ = false;
  bool cancelled_ = false;
  bool terminated_ = false;
  bool completed_ = false;
  bool completed_emitted_ = false;
  bool saw_error_ = false;
  bool saw_final_text_ = false;
  bool child_exited_ = false;
  bool killed_by_us_ = false;
  int exit_code_ = -1;
  std::string stdout_buffer_;
  std::string last_partial_text_;
  std::vector<std::byte> pending_chunk_;
  std::vector<RecognitionEvent> events_;
};

class CommandStreamingBackend : public AsrBackend {
public:
  explicit CommandStreamingBackend(
      CommandAsrProvider provider,
      vinput::notify::NotificationBridge* notification_bridge = nullptr)
      : provider_(std::move(provider)), notification_bridge_(notification_bridge) {}

  [[nodiscard]] BackendDescriptor Describe() const override {
    BackendDescriptor descriptor;
    descriptor.provider_id = provider_.id;
    descriptor.provider_type = vinput::asr::kCommandProviderType;
    descriptor.backend_id = "command-streaming";
    descriptor.capabilities.audio_delivery_mode = AudioDeliveryMode::Chunked;
    descriptor.capabilities.supports_streaming = true;
    descriptor.capabilities.supports_partial_results = true;
    descriptor.capabilities.requires_network = true;
    return descriptor;
  }

  std::unique_ptr<RecognitionSession> CreateSession(std::string* error) override {
    if (provider_.command.empty()) {
      if (error != nullptr) {
        *error = "Command ASR provider has empty command.";
      }
      return nullptr;
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<CommandStreamingSession>(provider_, provider_.id, notification_bridge_);
  }

private:
  CommandAsrProvider provider_;
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
};

} // namespace

std::unique_ptr<AsrBackend>
CreateCommandStreamingBackend(const CommandAsrProvider& provider, std::string* error,
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
  return std::make_unique<CommandStreamingBackend>(provider, notification_bridge);
}

} // namespace vinput::daemon::asr
