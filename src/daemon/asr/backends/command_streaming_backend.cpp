#include "daemon/asr/backends/command_streaming_backend.h"

#include <algorithm>
#include <atomic>
#include <base64.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fast_float/fast_float.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <glaze/core/common.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/json/json_access.h"
#include "common/notify/notification_bridge.h"
#include "common/registry/registry_i18n.h"
#include "common/utils/debug_log.h"
#include "common/utils/interpolation.h"
#include "common/utils/process_session.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/backends/command_backend_factory.h"
#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPollSliceMs = 50;
constexpr int kDefaultAwaitFinalTimeoutMs = 10000;
constexpr int kFinishGraceMarginMs = 2000;
constexpr int kMaxFinishGraceMs = 3600000;

bool HasTimedOut(Clock::time_point deadline) {
  return Clock::now() >= deadline;
}

std::optional<int> ParsePositiveSecondsMs(std::string_view raw) {
  const std::string_view trimmed = vinput::str::Trim(raw);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  double seconds = 0.0;
  const auto [ptr, ec] =
      fast_float::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), seconds);
  if (ec != std::errc{} || ptr != trimmed.data() + trimmed.size() || !std::isfinite(seconds) ||
      seconds <= 0.0) {
    return std::nullopt;
  }
  const double ms = seconds * 1000.0;
  if (ms >= static_cast<double>(kMaxFinishGraceMs)) {
    return kMaxFinishGraceMs;
  }
  return static_cast<int>(std::ceil(ms));
}

int ResolveFinishTimeoutMs(const CommandAsrProvider& provider) {
  int timeout_ms = provider.timeoutMs > 0 ? provider.timeoutMs : kDefaultAwaitFinalTimeoutMs;
  std::string grace_raw;
  if (const auto it = provider.env.find("VINPUT_ASR_FINISH_GRACE_SECS"); it != provider.env.end()) {
    grace_raw = vinput::interpolate::InterpolateEnv(it->second);
  } else if (const char* env_val = std::getenv("VINPUT_ASR_FINISH_GRACE_SECS")) {
    grace_raw = env_val;
  }
  if (const auto grace_ms = ParsePositiveSecondsMs(grace_raw)) {
    timeout_ms = std::max(timeout_ms, *grace_ms + kFinishGraceMarginMs);
  }
  return timeout_ms;
}

std::string MakeSessionSourceId(std::string_view provider_id) {
  static std::atomic<std::uint64_t> s_session_counter{0};
  return fmt::format("{}-{}", provider_id, ++s_session_counter);
}

class CommandStreamingSession : public RecognitionSession {
public:
  // CreateSession() must be able to start the child process eagerly so the
  // external provider handshake overlaps the microphone wake window (Issue #709).
  friend class CommandStreamingBackend;

  CommandStreamingSession(CommandAsrProvider provider, std::string provider_id,
                          int finish_timeout_ms,
                          vinput::notify::NotificationBridge* notification_bridge = nullptr)
      : provider_(std::move(provider)), provider_id_(std::move(provider_id)),
        finish_timeout_ms_(finish_timeout_ms), notification_bridge_(notification_bridge),
        source_id_(MakeSessionSourceId(provider_id_)) {}

  CommandStreamingSession(const CommandStreamingSession&) = delete;
  CommandStreamingSession& operator=(const CommandStreamingSession&) = delete;
  CommandStreamingSession(CommandStreamingSession&&) = delete;
  CommandStreamingSession& operator=(CommandStreamingSession&&) = delete;

  ~CommandStreamingSession() override { Terminate(); }

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    if (phase_ == Phase::Finishing || phase_ == Phase::Completed || phase_ == Phase::Cancelled) {
      if (error != nullptr) {
        *error = _("Recognition session already finished.");
      }
      return false;
    }

    if (!EnsureStarted(error)) {
      return false;
    }

    if (!pcm.empty()) {
      const auto byte_span = std::as_bytes(pcm);
      const glz::json_t payload = {
          {"type", "audio"},
          {"audio_base64", base64::encode_into<std::string>(byte_span.begin(), byte_span.end())},
      };
      if (!SendJsonLine(payload, error)) {
        return false;
      }
      ++sent_chunks_count_;
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
    if (phase_ == Phase::Cancelled) {
      if (error != nullptr) {
        *error = _("Recognition session was cancelled.");
      }
      return false;
    }
    if (phase_ == Phase::Completed) {
      if (error != nullptr) {
        if (!finish_success_ && !finish_error_.empty()) {
          *error = finish_error_;
        } else {
          error->clear();
        }
      }
      return finish_success_;
    }

    if (!StartFinish(error)) {
      return false;
    }
    if (!AwaitCompletion(error)) {
      return false;
    }
    return FinalizeFinish(error);
  }

  void Cancel() override {
    if (phase_ != Phase::Cancelled && phase_ != Phase::Completed) {
      const auto prev_phase = phase_;
      phase_ = Phase::Cancelled;
      if (prev_phase == Phase::Started || prev_phase == Phase::Finishing) {
        SendControlEvent("cancel", nullptr);
      }
    }
    finish_success_ = false;
    finish_error_ = _("Recognition session was cancelled.");
    stdout_buffer_.clear();
    last_partial_text_.clear();
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
  // Assigns the error when the caller supplied an out-parameter; always reports
  // failure so the teardown paths stay one-liners.
  static bool SetFinishError(std::string* error, std::string_view message) {
    if (error != nullptr) {
      *error = std::string(message);
    }
    return false;
  }

  // Sends the finish control event, closes stdin and arms the final-text
  // deadline. Caller must have guarded against the idempotent re-entry.
  bool StartFinish(std::string* error) {
    if (!EnsureStarted(error)) {
      return false;
    }

    if (!SendControlEvent("finish", error)) {
      const std::string err_text =
          error != nullptr && !error->empty() ? *error : _("failed to send finish event.");
      QueueError(err_text);
      phase_ = Phase::Completed;
      finish_success_ = false;
      finish_error_ = err_text;
      QueueCompleted();
      Terminate();
      return false;
    }

    // Only mark Finishing after the control event was sent: a failed send must
    // not look like a finished session (Issue #709).
    phase_ = Phase::Finishing;
    if (session_.has_value()) {
      session_->CloseStdin();
    }
    deadline_ = Clock::now() + std::chrono::milliseconds(finish_timeout_ms_);
    return true;
  }

  // Polls events until the provider closes the stream, completes, or exits.
  // Injects the poll/timeout failure into the event queue.
  bool AwaitCompletion(std::string* error) {
    while (!provider_closed_ && phase_ != Phase::Completed && !child_exited_) {
      if (!DrainEvents(std::chrono::milliseconds(kPollSliceMs), error)) {
        const std::string err_text =
            error != nullptr && !error->empty() ? *error : _("failed to poll provider.");
        QueueError(err_text);
        phase_ = Phase::Completed;
        finish_success_ = false;
        finish_error_ = err_text;
        QueueCompleted();
        Terminate();
        return false;
      }
      if (HasTimedOut(deadline_)) {
        QueueError(_("Streaming ASR provider timed out."));
        phase_ = Phase::Completed;
        finish_success_ = false;
        finish_error_ = _("Streaming ASR provider timed out.");
        QueueCompleted();
        Terminate();
        if (error != nullptr) {
          *error = _("Streaming ASR provider timed out.");
        }
        return false;
      }
    }
    return true;
  }

  // Drains leftovers, reports the no-text / interruption cases and resolves the
  // caller-visible outcome after the provider closes or exits.
  bool FinalizeFinish(std::string* error) {
    DrainEvents(std::chrono::milliseconds(0), nullptr);

    const bool cancelled = phase_ == Phase::Cancelled;
    if (!saw_final_text_ && !first_error_.has_value() && !cancelled && !killed_by_us_) {
      QueueError(_("Streaming ASR provider returned no text."));
    }
    phase_ = Phase::Completed;
    QueueCompleted();
    Terminate();

    if (cancelled || killed_by_us_ || interrupted_.load(std::memory_order_acquire)) {
      finish_success_ = false;
      finish_error_ = _("Recognition session was interrupted or cancelled.");
      return SetFinishError(error, finish_error_);
    }
    if (first_error_.has_value()) {
      finish_success_ = false;
      finish_error_ = *first_error_;
      return SetFinishError(error, finish_error_);
    }
    finish_success_ = true;
    finish_error_.clear();
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool HandleStdoutChunk(const std::vector<std::byte>& data, std::string* error) {
    stdout_buffer_.reserve(stdout_buffer_.size() + data.size());
    std::ranges::transform(data, std::back_inserter(stdout_buffer_),
                           [](std::byte b) { return static_cast<char>(b); });
    ConsumeStdoutLines(error);
    return error == nullptr || error->empty();
  }

  void HandleStderrChunk(const std::vector<std::byte>& data) {
    if (notification_bridge_ != nullptr) {
      if (!provider_title_.has_value()) {
        provider_title_ = vinput::registry::LoadCachedTitle(provider_id_);
      }
      notification_bridge_->FeedStderr(source_id_, data, *provider_title_);
    }
  }

  void HandleChildExited(const vinput::process::ChildEvent& event) {
    child_exited_ = true;
    exit_code_ = event.exit_code;
    killed_by_us_ = event.killed_by_us;
    if (notification_bridge_ != nullptr) {
      notification_bridge_->Flush(source_id_);
    }
    if (killed_by_us_) {
      VLOG_DEBUG("streaming ASR provider '{}' terminated by session cancellation", provider_id_);
    } else if (exit_code_ != 0 && !first_error_.has_value() &&
               (!saw_final_text_ || (phase_ == Phase::Finishing && !provider_closed_))) {
      VLOG_WARN("streaming ASR provider '{}' exited abnormally with code {}", provider_id_,
                exit_code_);
      QueueError(_("Streaming ASR provider exited abnormally."));
    }
  }

  bool EnsureStarted(std::string* error) {
    if (phase_ == Phase::Cancelled || interrupted_.load(std::memory_order_acquire)) {
      if (error != nullptr) {
        *error = _("Recognition session already cancelled.");
      }
      return false;
    }
    if (phase_ != Phase::Idle) {
      return true;
    }
    if (provider_.command.empty()) {
      if (error != nullptr) {
        *error = _("Command ASR provider has empty command.");
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
      QueueError(error != nullptr ? *error : _("failed to start."));
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
    phase_ = Phase::Started;
    return true;
  }

  bool SendControlEvent(std::string_view type, std::string* error) {
    const glz::json_t payload = {
        {"type", std::string(type)},
    };
    return SendJsonLine(payload, error);
  }

  bool RetryPushStdinWithBackoff(std::span<const std::byte> bytes, std::string* error) {
    constexpr auto kMaxWait = std::chrono::milliseconds(200);
    constexpr auto kRetrySlice = std::chrono::milliseconds(2);
    const auto deadline = std::chrono::steady_clock::now() + kMaxWait;

    while (std::chrono::steady_clock::now() < deadline) {
      if (!session_.has_value()) {
        return false;
      }
      int exit_code = -1;
      if (session_->HasExited(&exit_code)) {
        return false;
      }
      if (!DrainEvents(std::chrono::milliseconds(0), error) || !session_.has_value()) {
        return false;
      }
      std::this_thread::sleep_for(kRetrySlice);
      if (session_->PushStdin(bytes)) {
        return true;
      }
    }
    return false;
  }

  bool SendJsonLine(const glz::json_t& payload, std::string* error) {
    if (!session_.has_value()) {
      if (error != nullptr) {
        *error = _("provider session is not started.");
      }
      return false;
    }

    std::string line = vinput::json::ToText(payload);
    line.push_back('\n');
    const auto bytes = std::as_bytes(std::span(line));
    if (session_->PushStdin(bytes) || RetryPushStdinWithBackoff(bytes, error)) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    if (error != nullptr && !error->empty()) {
      return false;
    }

    DrainEvents(std::chrono::milliseconds(0), nullptr);
    if (error == nullptr || !session_.has_value()) {
      return false;
    }

    int exit_code = -1;
    if (session_->HasExited(&exit_code)) {
      *error =
          fmt::sprintf(_("Streaming ASR provider exited prematurely with code %d."), exit_code);
      return false;
    }

    const auto queued = session_->StdinQueueSizeApprox();
    *error = fmt::sprintf(
        _("Provider closed stdin or input buffer is full (queued_blocks=%zu, sent_chunks=%zu)."),
        queued, sent_chunks_count_);
    return false;
  }

  bool DrainEvents(std::chrono::milliseconds wait, std::string* error) {
    if (!session_.has_value()) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    auto& session = *session_;
    vinput::process::ChildEvent event;
    bool first = true;
    while (session.NextEvent(event, first ? wait : std::chrono::milliseconds(0))) {
      first = false;
      switch (event.kind) {
      case vinput::process::ChildEventKind::StdoutChunk: {
        if (!HandleStdoutChunk(event.data, error)) {
          return false;
        }
        break;
      }
      case vinput::process::ChildEventKind::StderrChunk: {
        HandleStderrChunk(event.data);
        break;
      }
      case vinput::process::ChildEventKind::Cancelled: {
        killed_by_us_ = true;
        break;
      }
      case vinput::process::ChildEventKind::Exited: {
        HandleChildExited(event);
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
        *error = _("invalid streaming provider JSON: ") + glz::format_error(ec, line);
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
    if (type == "final") {
      const std::string text =
          std::string(vinput::str::Trim(vinput::json::String(payload, "text")));
      if (!text.empty()) {
        saw_final_text_ = true;
        last_partial_text_.clear();
        events_.push_back({RecognitionEventKind::FinalText, text, {}});
      }
      return true;
    }
    if (type == "closed") {
      provider_closed_ = true;
      return true;
    }
    return true;
  }

  void QueueError(std::string_view error_text) {
    const std::string_view trimmed = vinput::str::Trim(error_text);
    std::string message = trimmed.empty() ? "failed." : std::string(trimmed);
    if (!first_error_.has_value()) {
      first_error_ = message;
    }
    events_.push_back({RecognitionEventKind::Error, {}, std::move(message)});
  }

  void QueueCompleted() {
    if (completed_emitted_) {
      return;
    }
    completed_emitted_ = true;
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

  // Lifecycle phase. The protocol booleans (started_/finished_/cancelled_/
  // completed_) collapsed into one mutually-exclusive state; one-shot and
  // cross-thread guards (terminated_, completed_emitted_, interrupted_) and
  // pure observations (saw_*, child_exited_, killed_by_us_) stay flags
  // (Issue #709).
  enum class Phase : std::uint8_t {
    Idle,      // Child not yet spawned.
    Started,   // Child running; audio is being streamed.
    Finishing, // "finish" control event sent; awaiting final text.
    Cancelled, // Cancel() called; being torn down.
    Completed, // Terminal state reached (provider closed or event queued).
  };

  CommandAsrProvider provider_;
  std::string provider_id_;
  int finish_timeout_ms_ = kDefaultAwaitFinalTimeoutMs;
  std::optional<std::string> provider_title_;
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
  std::string source_id_;
  Clock::time_point deadline_ = Clock::time_point::max();
  mutable std::mutex session_mutex_;
  std::optional<vinput::process::ProcessSession> session_;
  std::atomic<bool> interrupted_{false};
  Phase phase_ = Phase::Idle;
  bool provider_closed_ = false;
  bool finish_success_ = false;
  std::string finish_error_;
  bool terminated_ = false;
  bool completed_emitted_ = false;
  std::optional<std::string> first_error_;
  bool saw_final_text_ = false;
  bool child_exited_ = false;
  bool killed_by_us_ = false;
  int exit_code_ = -1;
  std::string stdout_buffer_;
  std::string last_partial_text_;
  std::size_t sent_chunks_count_ = 0;
  std::vector<RecognitionEvent> events_;
};

class CommandStreamingBackend : public AsrBackend {
public:
  explicit CommandStreamingBackend(
      CommandAsrProvider provider,
      vinput::notify::NotificationBridge* notification_bridge = nullptr)
      : provider_(std::move(provider)), finish_timeout_ms_(ResolveFinishTimeoutMs(provider_)),
        notification_bridge_(notification_bridge) {}

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
        *error = _("Command ASR provider has empty command.");
      }
      return nullptr;
    }

    // Start the external streaming subprocess immediately so the provider boot
    // and TLS/WebSocket handshake overlap the microphone wake window instead of
    // being paid serially on the first audio chunk (Issue #709).
    auto session = std::make_unique<CommandStreamingSession>(
        provider_, provider_.id, finish_timeout_ms_, notification_bridge_);
    if (!session->EnsureStarted(error)) {
      return nullptr;
    }
    if (error != nullptr) {
      error->clear();
    }
    return session;
  }

private:
  CommandAsrProvider provider_;
  int finish_timeout_ms_ = kDefaultAwaitFinalTimeoutMs;
  vinput::notify::NotificationBridge* notification_bridge_ = nullptr;
};

} // namespace

std::unique_ptr<AsrBackend>
CreateCommandStreamingBackend(const CommandAsrProvider& provider, std::string* error,
                              vinput::notify::NotificationBridge* notification_bridge) {
  return MakeCommandBackend<CommandStreamingBackend>(provider, error, notification_bridge);
}

} // namespace vinput::daemon::asr
