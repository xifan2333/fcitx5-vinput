#include "daemon/asr/backends/command_streaming_backend.h"

#include <algorithm>
#include <base64.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <span>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/utils/interpolation.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

extern char** environ;

namespace vinput::daemon::asr {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr int kPollSliceMs = 50;
constexpr int kDefaultWriteTimeoutMs = 5000;
constexpr int kMaxWriteTimeoutMs = 30000;
constexpr std::size_t kDrainBudgetBytes = std::size_t{64} * 1024;
constexpr int kAwaitFinalTimeoutMs = 10000;
constexpr int kFinalGraceMs = 200;

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void CloseIfOpen(int* fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

std::vector<char*> BuildArgv(const CommandAsrProvider& provider,
                             std::vector<std::string>* storage) {
  storage->clear();
  storage->reserve(provider.args.size() + 1);
  storage->push_back(provider.command);
  for (const auto& arg : provider.args) {
    storage->push_back(arg);
  }

  std::vector<char*> argv;
  argv.reserve(storage->size() + 1);
  for (auto& entry : *storage) {
    argv.push_back(entry.data());
  }
  argv.push_back(nullptr);
  return argv;
}

std::vector<char*> BuildEnvp(const std::map<std::string, std::string>& env,
                             std::vector<std::string>* storage) {
  storage->clear();

  for (char** entry = environ; entry && *entry; ++entry) {
    storage->push_back(*entry);
  }

  for (const auto& [key, value] : env) {
    if (key.empty()) {
      continue;
    }
    bool replaced = false;
    const std::string resolved_value = vinput::interpolate::InterpolateEnv(value);
    const std::string prefix = key + "=";
    for (auto& item : *storage) {
      if (item.rfind(prefix, 0) == 0) {
        item = prefix + resolved_value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      storage->push_back(prefix + resolved_value);
    }
  }

  std::vector<char*> envp;
  envp.reserve(storage->size() + 1);
  for (auto& entry : *storage) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);
  return envp;
}

std::string FormatProviderError(std::string detail, std::string_view fallback) {
  detail = std::string(vinput::str::Trim(detail));
  if (!detail.empty()) {
    return detail;
  }
  return std::string(fallback);
}

struct ChildProcess {
  pid_t pid = -1;
  int stdin_fd = -1;
  int stdout_fd = -1;
  int stderr_fd = -1;
  bool stdin_closed = false;
  bool child_exited = false;
  int exit_code = -1;
};

bool SpawnChild(const CommandAsrProvider& provider, ChildProcess* child, std::string* error) {
  if (!child) {
    if (error) {
      *error = "child process output is null";
    }
    return false;
  }
  child->pid = -1;
  child->stdin_fd = -1;
  child->stdout_fd = -1;
  child->stderr_fd = -1;
  child->stdin_closed = false;
  child->child_exited = false;
  child->exit_code = -1;

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (!vinput::process::CreateCloexecPipe(stdin_pipe) ||
      !vinput::process::CreateCloexecPipe(stdout_pipe) ||
      !vinput::process::CreateCloexecPipe(stderr_pipe)) {
    if (error) {
      *error = std::string("pipe failed: ") + std::strerror(errno);
    }
    CloseIfOpen(&stdin_pipe[0]);
    CloseIfOpen(&stdin_pipe[1]);
    CloseIfOpen(&stdout_pipe[0]);
    CloseIfOpen(&stdout_pipe[1]);
    CloseIfOpen(&stderr_pipe[0]);
    CloseIfOpen(&stderr_pipe[1]);
    return false;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);

  std::vector<std::string> argv_storage;
  std::vector<char*> argv = BuildArgv(provider, &argv_storage);
  std::vector<std::string> env_storage;
  std::vector<char*> envp = BuildEnvp(provider.env, &env_storage);

  pid_t pid = -1;
  const int rc =
      posix_spawnp(&pid, provider.command.c_str(), &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);

  CloseIfOpen(&stdin_pipe[0]);
  CloseIfOpen(&stdout_pipe[1]);
  CloseIfOpen(&stderr_pipe[1]);

  if (rc != 0) {
    if (error) {
      *error = std::string("failed to launch command: ") + std::strerror(rc);
    }
    CloseIfOpen(&stdin_pipe[1]);
    CloseIfOpen(&stdout_pipe[0]);
    CloseIfOpen(&stderr_pipe[0]);
    return false;
  }

  if (!SetNonBlocking(stdin_pipe[1]) || !SetNonBlocking(stdout_pipe[0]) ||
      !SetNonBlocking(stderr_pipe[0])) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    CloseIfOpen(&stdin_pipe[1]);
    CloseIfOpen(&stdout_pipe[0]);
    CloseIfOpen(&stderr_pipe[0]);
    if (error) {
      *error = std::string("fcntl failed: ") + std::strerror(errno);
    }
    return false;
  }

  child->pid = pid;
  child->stdin_fd = stdin_pipe[1];
  child->stdout_fd = stdout_pipe[0];
  child->stderr_fd = stderr_pipe[0];
  if (error) {
    error->clear();
  }
  return true;
}

bool HasTimedOut(Clock::time_point deadline) {
  return Clock::now() >= deadline;
}

int RemainingMs(Clock::time_point deadline, int fallback_ms) {
  return vinput::time::RemainingTimeoutMs(deadline, fallback_ms, Clock::now());
}

class CommandStreamingSession : public RecognitionSession {
public:
  CommandStreamingSession(const CommandAsrProvider& provider, std::string provider_id)
      : provider_(provider), provider_id_(std::move(provider_id)),
        deadline_(Clock::time_point::max()) {}

  ~CommandStreamingSession() override { Terminate(); }

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    if (finished_) {
      if (error) {
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
      const auto* bytes = reinterpret_cast<const std::byte*>(pcm.data());
      pending_chunk_.assign(bytes, bytes + pcm.size() * sizeof(int16_t));
    }

    if (!PumpIo(0, error)) {
      return false;
    }

    if (error) {
      error->clear();
    }
    return true;
  }

  bool Finish(std::string* error) override {
    if (finished_) {
      if (error) {
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
    CloseIfOpen(&child_.stdin_fd);
    child_.stdin_closed = true;
    deadline_ = Clock::now() + std::chrono::milliseconds(kAwaitFinalTimeoutMs);

    while (!saw_final_text_ && !completed_ && !child_.child_exited) {
      if (!PumpIo(kPollSliceMs, error)) {
        QueueError(error != nullptr && !error->empty() ? *error : "failed to poll provider.");
        QueueCompleted();
        Terminate();
        return false;
      }
      if (HasTimedOut(deadline_)) {
        QueueError(FormatProviderError(std::move(stderr_tail_), "timed out."));
        QueueCompleted();
        Terminate();
        if (error) {
          *error = events_.back().error;
        }
        return false;
      }
    }

    bool grace_pump_failed = false;
    if (saw_final_text_) {
      const Clock::time_point grace_deadline =
          Clock::now() + std::chrono::milliseconds(kFinalGraceMs);
      while (!HasTimedOut(grace_deadline) && !completed_ && !child_.child_exited) {
        if (!PumpIo(RemainingMs(grace_deadline, kPollSliceMs), error)) {
          grace_pump_failed = true;
          break;
        }
      }
    }

    PumpIo(0, nullptr);

    if (!saw_final_text_ && !saw_error_) {
      QueueError(FormatProviderError(std::move(stderr_tail_), "returned no text."));
    }
    QueueCompleted();
    Terminate();

    if (error) {
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
      // Cancel does not start a new child. For a running provider the
      // notification is best-effort and is itself subject to a finite deadline.
      if (started_ && !child_.stdin_closed && child_.stdin_fd >= 0) {
        SendControlEvent("cancel", nullptr);
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
  bool EnsureStarted(std::string* error) {
    if (cancelled_) {
      if (error != nullptr) {
        *error = "Recognition session already cancelled.";
      }
      return false;
    }
    if (started_) {
      return true;
    }
    if (provider_.command.empty()) {
      if (error) {
        *error = "Command ASR provider has empty command.";
      }
      return false;
    }
    if (!SpawnChild(provider_, &child_, error)) {
      QueueError(error ? *error : "failed to start.");
      QueueCompleted();
      return false;
    }
    started_ = true;
    return true;
  }

  bool FlushPendingChunk(bool commit, std::string* error) {
    if (pending_chunk_.empty()) {
      if (error) {
        error->clear();
      }
      return true;
    }

    json payload = {
        {"type", "audio"},
        {"audio_base64",
         base64::encode_into<std::string>(pending_chunk_.begin(), pending_chunk_.end())},
        {"commit", commit},
    };
    pending_chunk_.clear();
    return SendJsonLine(payload, error);
  }

  bool SendControlEvent(std::string_view type, std::string* error) {
    json payload = {
        {"type", type},
    };
    return SendJsonLine(payload, error);
  }

  bool SendJsonLine(const json& payload, std::string* error) {
    if (child_.stdin_closed || child_.stdin_fd < 0) {
      if (error) {
        *error = "provider stdin is already closed.";
      }
      return false;
    }

    std::string line = payload.dump();
    line.push_back('\n');
    std::size_t offset = 0;
    // Each outbound message has its own deadline. A prolonged recording
    // must not be limited by the budget of its first chunk.
    const int write_timeout_ms = provider_.timeoutMs > 0
                                     ? std::min(provider_.timeoutMs, kMaxWriteTimeoutMs)
                                     : kDefaultWriteTimeoutMs;
    const auto write_deadline = Clock::now() + std::chrono::milliseconds(write_timeout_ms);
    while (offset < line.size()) {
      if (HasTimedOut(write_deadline) || HasTimedOut(deadline_)) {
        if (error) {
          *error = FormatProviderError(std::move(stderr_tail_), "provider write timed out.");
        }
        return false;
      }

      pollfd fd{};
      fd.fd = child_.stdin_fd;
      fd.events = POLLOUT;
      const auto deadline = std::min(write_deadline, deadline_);
      const int rc = poll(&fd, 1, RemainingMs(deadline, kPollSliceMs));
      if (rc < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (error) {
          *error = std::string("poll failed: ") + std::strerror(errno);
        }
        return false;
      }
      if (rc == 0) {
        // Providers may emit output before reading more stdin; draining it
        // (bounded per call in PumpIo/Drain*) keeps both pipe directions
        // moving without extending the absolute write deadline.
        PumpIo(0, nullptr);
        continue;
      }

      if ((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        if (error) {
          *error = FormatProviderError(std::move(stderr_tail_), "provider closed stdin.");
        }
        return false;
      }
      if ((fd.revents & POLLOUT) == 0) {
        continue;
      }

      const ssize_t written = write(child_.stdin_fd, line.data() + offset, line.size() - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        continue;
      }
      if (error) {
        *error = std::string("write failed: ") + std::strerror(errno);
      }
      return false;
    }

    if (error) {
      error->clear();
    }
    return true;
  }

  bool PumpIo(int poll_timeout_ms, std::string* error) {
    if (!started_) {
      if (error) {
        error->clear();
      }
      return true;
    }

    ReapChild();

    pollfd fds[2];
    nfds_t nfds = 0;
    if (child_.stdout_fd >= 0) {
      fds[nfds].fd = child_.stdout_fd;
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      ++nfds;
    }
    if (child_.stderr_fd >= 0) {
      fds[nfds].fd = child_.stderr_fd;
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      ++nfds;
    }

    const int timeout_ms = HasTimedOut(deadline_) ? 0 : RemainingMs(deadline_, poll_timeout_ms);
    // Once both output pipes reach EOF, Finish may still be waiting for the
    // provider to exit. A bounded wait prevents a tight waitpid(WNOHANG) loop.
    // Preserve PumpIo(0)'s nonblocking behavior for live audio ingestion.
    if (nfds == 0 && !child_.child_exited && timeout_ms > 0) {
      const int rc = poll(nullptr, 0, timeout_ms);
      if (rc < 0 && errno != EINTR) {
        if (error != nullptr) {
          *error = std::string("poll failed: ") + std::strerror(errno);
        }
        return false;
      }
    }
    if (nfds > 0) {
      const int rc = poll(fds, nfds, timeout_ms);
      if (rc < 0 && errno != EINTR) {
        if (error) {
          *error = std::string("poll failed: ") + std::strerror(errno);
        }
        return false;
      }

      for (nfds_t i = 0; i < nfds; ++i) {
        if ((fds[i].revents & POLLIN) == 0 &&
            (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) == 0) {
          continue;
        }
        if (fds[i].fd == child_.stdout_fd) {
          if (!DrainStdout(error)) {
            return false;
          }
        } else if (fds[i].fd == child_.stderr_fd) {
          DrainStderr();
        }
      }
    }

    ReapChild();
    if (child_.child_exited && child_.exit_code != 0 && !saw_error_ && !saw_final_text_) {
      QueueError(FormatProviderError(std::move(stderr_tail_), "failed."));
    }
    if (error) {
      error->clear();
    }
    return true;
  }

  bool DrainStdout(std::string* error) {
    char buffer[4096];
    std::size_t remaining_budget = kDrainBudgetBytes;
    while (remaining_budget > 0) {
      const ssize_t n = read(child_.stdout_fd, buffer, std::min(sizeof(buffer), remaining_budget));
      if (n > 0) {
        remaining_budget -= static_cast<std::size_t>(n);
        stdout_buffer_.append(buffer, static_cast<std::size_t>(n));
        ConsumeStdoutLines(error);
        if (error && !error->empty()) {
          return false;
        }
        continue;
      }
      if (n == 0) {
        CloseIfOpen(&child_.stdout_fd);
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      if (error) {
        *error = std::string("read stdout failed: ") + std::strerror(errno);
      }
      return false;
    }
    return true;
  }

  void DrainStderr() {
    char buffer[4096];
    std::size_t remaining_budget = kDrainBudgetBytes;
    while (remaining_budget > 0) {
      const ssize_t n = read(child_.stderr_fd, buffer, std::min(sizeof(buffer), remaining_budget));
      if (n > 0) {
        remaining_budget -= static_cast<std::size_t>(n);
        stderr_buffer_.append(buffer, static_cast<std::size_t>(n));
        ConsumeStderrLines();
        continue;
      }
      if (n == 0) {
        ConsumeStderrLines(true);
        CloseIfOpen(&child_.stderr_fd);
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      break;
    }
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

  void ConsumeStderrLines(bool flush_tail = false) {
    vinput::process::DrainLinesFromBuffer(
        stderr_buffer_,
        [&](std::string_view raw_line) {
          const std::string line(vinput::str::Trim(raw_line));
          if (line.empty()) {
            return;
          }
          stderr_tail_ = line;
          std::fprintf(stderr, "vinput: command streaming stderr: %s\n", line.c_str());
        },
        flush_tail);
  }

  bool HandleOutputLine(const std::string& line, std::string* error) {
    json payload;
    try {
      payload = json::parse(line);
    } catch (const std::exception& ex) {
      if (error) {
        *error = std::string("invalid streaming provider JSON: ") + ex.what();
      }
      return false;
    }

    const std::string type = payload.value("type", std::string{});
    if (type == "session_started" || type.empty()) {
      return true;
    }
    if (type == "partial") {
      const std::string text = std::string(vinput::str::Trim(payload.value("text", std::string{})));
      if (!text.empty() && text != last_partial_text_) {
        last_partial_text_ = text;
        events_.push_back({RecognitionEventKind::PartialText, text, {}});
      }
      return true;
    }
    if (type == "final" || type == "final_timestamps") {
      const std::string text = std::string(vinput::str::Trim(payload.value("text", std::string{})));
      if (!text.empty()) {
        saw_final_text_ = true;
        last_partial_text_.clear();
        events_.push_back({RecognitionEventKind::FinalText, text, {}});
      }
      return true;
    }
    if (type == "error") {
      QueueError(std::string(vinput::str::Trim(payload.value("message", std::string{}))));
      return true;
    }
    if (type == "closed") {
      completed_ = true;
      return true;
    }
    return true;
  }

  void QueueError(std::string error_text) {
    error_text = FormatProviderError(std::move(error_text), "failed.");
    saw_error_ = true;
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

  void ReapChild() {
    if (child_.pid <= 0 || child_.child_exited) {
      return;
    }
    int status = 0;
    const pid_t rc = waitpid(child_.pid, &status, WNOHANG);
    if (rc != child_.pid) {
      return;
    }
    child_.child_exited = true;
    if (WIFEXITED(status)) {
      child_.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      child_.exit_code = 128 + WTERMSIG(status);
    } else {
      child_.exit_code = -1;
    }
    ConsumeStderrLines(true);
  }

  void Terminate() {
    if (terminated_) {
      return;
    }
    terminated_ = true;

    CloseIfOpen(&child_.stdin_fd);
    CloseIfOpen(&child_.stdout_fd);
    CloseIfOpen(&child_.stderr_fd);

    ReapChild();
    if (child_.pid > 0 && !child_.child_exited) {
      kill(child_.pid, SIGKILL);
      pid_t wait_rc = waitpid(child_.pid, nullptr, 0);
      while (wait_rc < 0 && errno == EINTR) {
        wait_rc = waitpid(child_.pid, nullptr, 0);
      }
      if (wait_rc == child_.pid || (wait_rc < 0 && errno == ECHILD)) {
        child_.child_exited = true;
        child_.exit_code = -1;
      }
    }
  }

  CommandAsrProvider provider_;
  std::string provider_id_;
  Clock::time_point deadline_{};
  ChildProcess child_;
  bool started_ = false;
  bool finished_ = false;
  bool cancelled_ = false;
  bool terminated_ = false;
  bool completed_ = false;
  bool completed_emitted_ = false;
  bool saw_error_ = false;
  bool saw_final_text_ = false;
  std::string stdout_buffer_;
  std::string stderr_buffer_;
  std::string stderr_tail_;
  std::string last_partial_text_;
  std::vector<std::byte> pending_chunk_;
  std::vector<RecognitionEvent> events_;
};

class CommandStreamingBackend : public AsrBackend {
public:
  explicit CommandStreamingBackend(CommandAsrProvider provider) : provider_(std::move(provider)) {}

  BackendDescriptor Describe() const override {
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
      if (error) {
        *error = "Command ASR provider has empty command.";
      }
      return nullptr;
    }
    if (error) {
      error->clear();
    }
    return std::make_unique<CommandStreamingSession>(provider_, provider_.id);
  }

private:
  CommandAsrProvider provider_;
};

} // namespace

std::unique_ptr<AsrBackend> CreateCommandStreamingBackend(const CommandAsrProvider& provider,
                                                          std::string* error) {
  if (provider.command.empty()) {
    if (error) {
      *error = "Command ASR provider has empty command.";
    }
    return nullptr;
  }
  if (error) {
    error->clear();
  }
  return std::make_unique<CommandStreamingBackend>(provider);
}

} // namespace vinput::daemon::asr
