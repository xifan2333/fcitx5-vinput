#include "daemon/asr/backends/command_streaming_backend.h"

#include <algorithm>
#include <base64.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <glaze/core/common.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
#include "common/utils/debug_log.h"
#include "common/utils/process_session.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPollSlice = std::chrono::milliseconds(50);

// Upper bound on the events one pump call delivers before returning to its caller, so the caller's
// own deadlines stay in charge.
constexpr std::size_t kMaxEventsPerPump = 64;
constexpr int kDefaultWriteTimeoutMs = 5000;
constexpr int kMaxWriteTimeoutMs = 30000;

// How long a provider may take to answer `finish` with its final text, and how long it then gets to
// flush whatever else it printed.
constexpr auto kAwaitFinalTimeout = std::chrono::milliseconds(10000);
constexpr auto kFinalGrace = std::chrono::milliseconds(200);

// How long the provider gets to be *told* about a cancel. It is a courtesy message: a provider that
// stopped reading must not delay the termination it is being told about.
constexpr auto kCancelNotifyTimeout = std::chrono::milliseconds(200);

// Cancelling has to leave a reaped child behind before it returns, because the caller's next step
// is to drop the session. The pump's own escalation is bounded, so this only bounds the handshake.
constexpr auto kCancelReapTimeout = std::chrono::milliseconds(3000);

// stderr is diagnostics only, but it is the only explanation a failing provider gives, so every
// line is kept for the error hint. The budget keeps a chatty provider from turning a notification
// into a wall of text; whole lines are dropped from the oldest end.
constexpr std::size_t kMaxStderrHintBytes = std::size_t{1024};

// A provider failure reads as the reason this daemon observed, followed by whatever the provider
// printed: the reason alone hides the provider's own explanation, and the provider's output alone
// leaves the caller guessing what went wrong.
std::string FormatProviderError(std::string_view reason, std::string detail) {
  detail = std::string(vinput::str::Trim(detail));
  if (detail.empty()) {
    return std::string(reason);
  }
  return std::format("{}: {}", reason, detail);
}

// The session reports raw bytes; the protocol on top of it is text. This is where the two meet.
void AppendText(std::string* out, const std::vector<std::byte>& data) {
  out->reserve(out->size() + data.size());
  for (const std::byte value : data) {
    out->push_back(static_cast<char>(value));
  }
}

class CommandStreamingSession : public RecognitionSession {
public:
  explicit CommandStreamingSession(CommandAsrProvider provider) : provider_(std::move(provider)) {}

  CommandStreamingSession(const CommandStreamingSession&) = delete;
  CommandStreamingSession& operator=(const CommandStreamingSession&) = delete;
  CommandStreamingSession(CommandStreamingSession&&) = delete;
  CommandStreamingSession& operator=(CommandStreamingSession&&) = delete;

  ~CommandStreamingSession() override { Terminate(); }

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    if (finished_) {
      SetError(error, "Recognition session already finished.");
      return false;
    }
    if (!EnsureStarted(error)) {
      return false;
    }

    // The chunk boundary is what the provider sees as one JSON line: the previous chunk is
    // committed now, the new one waits for the next call.
    if (!FlushPendingChunk(false, error)) {
      return false;
    }
    if (!pcm.empty()) {
      const auto bytes = std::as_bytes(pcm);
      pending_chunk_.assign(bytes.begin(), bytes.end());
    }

    PumpEvents(std::chrono::milliseconds::zero());
    SetError(error, {});
    return true;
  }

  bool Finish(std::string* error) override {
    if (finished_) {
      SetError(error, {});
      return true;
    }
    if (!EnsureStarted(error)) {
      return false;
    }
    finished_ = true;

    if (!FlushPendingChunk(true, error)) {
      FailPending(error, "failed to flush the last audio chunk.");
      return false;
    }
    if (!SendControlEvent("finish", error)) {
      FailPending(error, "failed to send the finish event.");
      return false;
    }
    session_->CloseStdin();

    const Clock::time_point deadline = Clock::now() + kAwaitFinalTimeout;
    while (!saw_final_text_ && !completed_ && !child_exited_) {
      if (Clock::now() >= deadline) {
        QueueError(FormatProviderError("timed out.", StderrHint()));
        break;
      }
      PumpEvents(kPollSlice);
    }

    // A provider that already published its final text may still be flushing its tail, but it does
    // not get to hold the caller here.
    if (saw_final_text_ && !completed_ && !child_exited_) {
      const Clock::time_point grace_deadline = Clock::now() + kFinalGrace;
      while (!completed_ && !child_exited_ && Clock::now() < grace_deadline) {
        PumpEvents(kPollSlice);
      }
    }

    PumpEvents(std::chrono::milliseconds::zero());
    if (!saw_final_text_ && !saw_error_) {
      QueueError(FormatProviderError("returned no text.", StderrHint()));
    }
    QueueCompleted();
    Terminate();

    if (saw_error_) {
      SetError(error, last_error_);
      return false;
    }
    SetError(error, {});
    return true;
  }

  void Cancel() override {
    if (cancelled_) {
      return;
    }
    cancelled_ = true;
    if (session_ && !finished_) {
      // Best effort: telling the provider lets it abort its upstream session cleanly, but it gets a
      // short deadline of its own, because the termination it is being told about must not wait for
      // it.
      (void)SendControlEvent("cancel", nullptr, kCancelNotifyTimeout);
      session_->Cancel();
      // The child must be gone before this returns: the caller is about to drop the session, and a
      // provider still holding the microphone's successor would be worse than a slow cancel.
      const Clock::time_point deadline = Clock::now() + kCancelReapTimeout;
      while (!child_exited_ && Clock::now() < deadline) {
        PumpEvents(kPollSlice);
      }
    }
    QueueCompleted();
    Terminate();
  }

  std::vector<RecognitionEvent> PollEvents() override {
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

private:
  static void SetError(std::string* error, std::string_view message) {
    if (error != nullptr) {
      *error = std::string(message);
    }
  }

  [[nodiscard]] int WriteTimeoutMs() const {
    return provider_.timeoutMs > 0 ? std::min(provider_.timeoutMs, kMaxWriteTimeoutMs)
                                   : kDefaultWriteTimeoutMs;
  }

  bool EnsureStarted(std::string* error) {
    if (cancelled_) {
      SetError(error, "Recognition session already cancelled.");
      return false;
    }
    if (session_) {
      return true;
    }
    if (provider_.command.empty()) {
      SetError(error, "Command ASR provider has empty command.");
      return false;
    }

    vinput::process::CommandSpec spec;
    spec.command = provider_.command;
    spec.args = provider_.args;
    spec.env = provider_.env;
    auto session = vinput::process::ProcessSession::Start(spec);
    if (!session.has_value()) {
      QueueError(session.error());
      QueueCompleted();
      SetError(error, session.error());
      return false;
    }
    session_ = std::make_unique<vinput::process::ProcessSession>(std::move(*session));
    return true;
  }

  bool FlushPendingChunk(bool commit, std::string* error) {
    if (pending_chunk_.empty()) {
      SetError(error, {});
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

  bool SendControlEvent(std::string_view type, std::string* error,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
    const glz::json_t payload = {
        {"type", std::string(type)},
    };
    return SendJsonLine(payload, error, timeout);
  }

  bool SendJsonLine(const glz::json_t& payload, std::string* error,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
    std::string line = vinput::json::ToText(payload);
    line.push_back('\n');
    return SendBytes(line, error, timeout);
  }

  // Hands `line` to the provider and waits until it has actually been written. Acceptance into the
  // session's queue is not delivery: a provider that stopped reading has to surface as a write
  // failure within its own deadline, not as a queue that quietly fills up.
  bool SendBytes(std::string_view line, std::string* error,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
    if (!session_) {
      SetError(error, "provider is not running.");
      return false;
    }

    const auto bytes = std::as_bytes(std::span<const char>(line.data(), line.size()));
    const auto budget = timeout > std::chrono::milliseconds::zero()
                            ? timeout
                            : std::chrono::milliseconds(WriteTimeoutMs());
    const Clock::time_point deadline = Clock::now() + budget;
    bool accepted = session_->PushStdin(bytes);
    while (true) {
      const vinput::process::StdinState state = session_->Stdin();
      if (accepted && state == vinput::process::StdinState::Flushed) {
        SetError(error, {});
        return true;
      }
      // Accepted but closed, or the child is gone: the block was not delivered, and waiting for the
      // deadline would only delay the report.
      if (state == vinput::process::StdinState::Closed || child_exited_) {
        SetError(error, FormatProviderError("provider stopped reading its input.", StderrHint()));
        return false;
      }
      if (Clock::now() >= deadline) {
        SetError(error, FormatProviderError("provider write timed out.", StderrHint()));
        return false;
      }
      if (!accepted) {
        // The queue had no room; the pump makes room from its side, so give it a slice first.
        PumpEvents(kPollSlice);
        accepted = session_->PushStdin(bytes);
        continue;
      }
      PumpEvents(kPollSlice);
    }
  }

  void FailPending(std::string* error, std::string_view fallback) {
    const std::string message =
        error != nullptr && !error->empty() ? *error : std::string(fallback);
    QueueError(message);
    QueueCompleted();
    Terminate();
    SetError(error, message);
  }

  // Delivers what the child has already produced, waiting at most `wait` for the first of it. The
  // session owns the pipes and the timers; this only translates.
  void PumpEvents(std::chrono::milliseconds wait) {
    if (!session_) {
      return;
    }
    vinput::process::ChildEvent event;
    if (!session_->NextEvent(event, wait)) {
      return;
    }
    HandleChildEvent(event);
    DrainEvents();
  }

  // Drains what is already queued. While the child still runs the drain is capped, so a provider
  // that keeps producing cannot hold this call - and with it the caller's write or finish deadline
  // - inside the loop. Once the session has finished, every event is already published: the queue
  // is finite, and draining all of it is what keeps a real `final` from being left behind and read
  // as "no text".
  void DrainEvents() {
    if (!session_) {
      return;
    }
    const bool unbounded = session_->Finished();
    vinput::process::ChildEvent event;
    for (std::size_t drained = 0; unbounded || drained < kMaxEventsPerPump; ++drained) {
      if (!session_->NextEvent(event, std::chrono::milliseconds::zero())) {
        return;
      }
      HandleChildEvent(event);
    }
  }

  void HandleChildEvent(const vinput::process::ChildEvent& event) {
    switch (event.kind) {
    case vinput::process::ChildEventKind::StdoutChunk:
      AppendText(&stdout_buffer_, event.data);
      ConsumeStdoutLines();
      break;
    case vinput::process::ChildEventKind::StderrChunk:
      AppendText(&stderr_buffer_, event.data);
      ConsumeStderrLines(false);
      break;
    case vinput::process::ChildEventKind::Cancelled:
      // Bookkeeping only: the terminal `Exited` follows and carries the status.
      break;
    case vinput::process::ChildEventKind::Exited:
      child_exited_ = true;
      exit_code_ = event.exit_code;
      ConsumeStderrLines(true);
      // A cancel ends the child through SIGTERM, so its status is non-zero by construction:
      // reporting that as a provider failure would turn "the user stopped dictating" into an error
      // notification.
      const bool cancelled_by_us = cancelled_ || event.killed_by_us;
      if (exit_code_ != 0 && !cancelled_by_us && !saw_error_ && !saw_final_text_) {
        QueueError(FormatProviderError("failed.", StderrHint()));
      }
      break;
    }
  }

  void ConsumeStdoutLines() {
    vinput::process::DrainLinesFromBuffer(stdout_buffer_, [&](std::string_view raw_line) {
      const std::string line(vinput::str::Trim(raw_line));
      if (line.empty()) {
        return;
      }
      HandleOutputLine(line);
    });
  }

  void ConsumeStderrLines(bool flush_tail) {
    vinput::process::DrainLinesFromBuffer(
        stderr_buffer_,
        [&](std::string_view raw_line) {
          const std::string line(vinput::str::Trim(raw_line));
          if (line.empty()) {
            return;
          }
          VLOG_DEBUG("command streaming stderr: {}", line);
          AppendStderrLine(line);
        },
        flush_tail);
  }

  // Every diagnostic line becomes part of the hint. A provider's fatal line is usually its last
  // one, but the lines before it are what explain it, and a line printed after the incident is
  // noise - so keeping only the last line both loses information and can pick the wrong line.
  void AppendStderrLine(std::string_view line) {
    if (!stderr_hint_.empty()) {
      stderr_hint_.push_back('\n');
    }
    stderr_hint_.append(line);
    if (stderr_hint_.size() <= kMaxStderrHintBytes) {
      return;
    }
    const std::size_t overflow = stderr_hint_.size() - kMaxStderrHintBytes;
    const std::size_t next_line = stderr_hint_.find('\n', overflow);
    stderr_hint_.erase(0, next_line == std::string::npos ? overflow : next_line + 1);
  }

  [[nodiscard]] std::string StderrHint() const { return stderr_hint_; }

  void HandleOutputLine(const std::string& line) {
    glz::json_t payload;
    if (const auto ec = glz::read_json(payload, line); bool(ec)) {
      QueueError(std::string("invalid streaming provider JSON: ") + glz::format_error(ec, line));
      return;
    }

    const std::string type = vinput::json::String(payload, "type");
    if (type == "session_started" || type.empty()) {
      return;
    }
    if (type == "partial") {
      const std::string text =
          std::string(vinput::str::Trim(vinput::json::String(payload, "text")));
      if (!text.empty() && text != last_partial_text_) {
        last_partial_text_ = text;
        events_.push_back({RecognitionEventKind::PartialText, text, {}});
      }
      return;
    }
    if (type == "final" || type == "final_timestamps") {
      const std::string text =
          std::string(vinput::str::Trim(vinput::json::String(payload, "text")));
      if (!text.empty()) {
        saw_final_text_ = true;
        last_partial_text_.clear();
        events_.push_back({RecognitionEventKind::FinalText, text, {}});
      }
      return;
    }
    if (type == "error") {
      QueueError(std::string(vinput::str::Trim(vinput::json::String(payload, "message"))));
      return;
    }
    if (type == "closed") {
      completed_ = true;
    }
  }

  void QueueError(std::string error_text) {
    error_text = FormatProviderError(std::move(error_text), "failed.");
    saw_error_ = true;
    last_error_ = error_text;
    events_.push_back({RecognitionEventKind::Error, {}, std::move(error_text)});
  }

  void QueueCompleted() {
    if (completed_emitted_) {
      return;
    }
    completed_emitted_ = true;
    completed_ = true;
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
  }

  // The session is the only owner of the child: dropping it cancels, joins the pump and reaps, all
  // of which is bounded by the module itself.
  void Terminate() {
    session_.reset();
    child_exited_ = true;
  }

  CommandAsrProvider provider_;
  std::unique_ptr<vinput::process::ProcessSession> session_;
  bool finished_ = false;
  bool cancelled_ = false;
  bool completed_ = false;
  bool completed_emitted_ = false;
  bool saw_error_ = false;
  bool saw_final_text_ = false;
  bool child_exited_ = false;
  int exit_code_ = -1;
  std::string last_error_;
  std::string stdout_buffer_;
  std::string stderr_buffer_;
  std::string stderr_hint_;
  std::string last_partial_text_;
  std::vector<std::byte> pending_chunk_;
  std::vector<RecognitionEvent> events_;
};

class CommandStreamingBackend : public AsrBackend {
public:
  explicit CommandStreamingBackend(CommandAsrProvider provider) : provider_(std::move(provider)) {}

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
      SetError(error, "Command ASR provider has empty command.");
      return nullptr;
    }
    SetError(error, {});
    return std::make_unique<CommandStreamingSession>(provider_);
  }

private:
  static void SetError(std::string* error, std::string_view message) {
    if (error != nullptr) {
      *error = std::string(message);
    }
  }

  CommandAsrProvider provider_;
};

} // namespace

std::unique_ptr<AsrBackend> CreateCommandStreamingBackend(const CommandAsrProvider& provider,
                                                          std::string* error) {
  if (provider.command.empty()) {
    if (error != nullptr) {
      *error = "Command ASR provider has empty command.";
    }
    return nullptr;
  }
  if (error != nullptr) {
    error->clear();
  }
  return std::make_unique<CommandStreamingBackend>(provider);
}

} // namespace vinput::daemon::asr
