#include "common/utils/process_session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <readerwriterqueue.h>
#include <span>
#include <string>
#include <string_view>
#include <subprocess.hpp>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <signal.h> // SIGKILL, SIGTERM
}

#include "common/utils/debug_log.h"
#include "common/utils/expected.h"
#include "common/utils/process_internal.h"
#include "common/utils/process_utils.h"

namespace vinput::process {

namespace {

using Clock = std::chrono::steady_clock;

// How long one poll() waits before the pump re-examines its own bookkeeping: stdin that arrived, a
// cancel that was signalled, a terminated group that has not been reaped yet.
constexpr auto kPollSlice = std::chrono::milliseconds(50);

// A reaped child can still leave bytes in its pipes, so they are drained for a bounded window
// instead of being dropped mid-line.
constexpr auto kPostExitDrain = std::chrono::milliseconds(200);

// Cancel escalation. A provider script gets a moment to unwind on SIGTERM before its whole process
// group is killed, and a killed group gets a bounded window to be reaped.
constexpr auto kTerminateGrace = std::chrono::milliseconds(300);
constexpr auto kForceGrace = std::chrono::milliseconds(1000);

// stdin crosses the thread boundary in whole blocks. The bound is what turns a provider that stops
// reading into a reported write failure instead of unbounded memory growth.
constexpr std::size_t kStdinQueueCapacity = 64;

// The event queue's first block. Further blocks are allocated on demand, so backpressure comes from
// kMaxQueuedOutputBytes alone: the queue itself must never refuse an event, because the terminal
// one is how the caller learns the child is gone. A small first block also keeps the growth path
// cheap to reach in tests.
constexpr std::size_t kEventQueueCapacity = 16;

// Output is bounded by bytes rather than by event count: at the cap the pump stops draining and
// lets the pipe apply its own backpressure, so a provider that prints without end cannot outrun a
// caller that has stopped consuming.
constexpr std::size_t kMaxQueuedOutputBytes = std::size_t{1024} * 1024;

constexpr auto kZeroMs = std::chrono::milliseconds::zero();

// Consumes the pending count of an eventfd. Zero means the descriptor was not readable.
std::uint64_t DrainEventFd(int fd) {
  std::uint64_t count = 0;
  const ssize_t n = ::read(fd, &count, sizeof(count));
  return std::cmp_equal(n, sizeof(count)) ? count : 0;
}

// One pending notification is all a waiter needs, so a saturated counter is not an error: either
// way the waiter observes that something happened.
void SignalEventFd(int fd) noexcept {
  const std::uint64_t one = 1;
  const ssize_t written = ::write(fd, &one, sizeof(one));
  (void)written;
}

} // namespace

// The pump thread's world: the child, its three pipes, and the two queues that cross the thread
// boundary. Every descriptor, every channel flag and the reaped flag below are touched only by the
// pump thread, which is what lets Cancel() be a bare eventfd write from any thread.
struct ProcessSession::Impl {
  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() { Shutdown(); }

  bool Open(const CommandSpec& spec, std::string* error);
  void StartPump() {
    pump_ = std::thread([this] { Pump(); });
  }
  void Shutdown() noexcept;

  bool PushStdin(std::span<const std::byte> bytes);
  void CloseStdin();
  bool NextEvent(ChildEvent& out, std::chrono::milliseconds wait);
  void Cancel() const noexcept;
  [[nodiscard]] int Pid() const noexcept { return proc_.has_value() ? proc_->pid() : -1; }

private:
  void Pump();
  void QueueEvent(ChildEvent&& event);
  bool PopEvent(ChildEvent& out);

  // Channel bookkeeping is centralised so the poll set and the released descriptors can never
  // disagree about what is still live.
  void CloseStdinChannel();
  void CloseOutputChannel(bool is_stdout);
  detail::DrainOutcome DrainOutput(int fd, ChildEventKind kind);
  detail::DrainOutcome ServiceOutputChannel(bool is_stdout);

  moodycamel::ReaderWriterQueue<std::vector<std::byte>> stdin_queue_{kStdinQueueCapacity};
  moodycamel::ReaderWriterQueue<ChildEvent> event_queue_{kEventQueueCapacity};
  std::atomic<std::size_t> queued_output_bytes_{0};
  std::thread pump_;
  std::optional<subprocess::Popen> proc_;

  // Pump-thread owned. -1 marks a descriptor the pump has already released.
  int in_fd_ = -1;
  int out_fd_ = -1;
  int err_fd_ = -1;
  int exit_code_ = -1;
  bool stdin_channel_open_ = false;
  bool stdout_channel_open_ = false;
  bool stderr_channel_open_ = false;
  std::vector<std::byte> pending_;
  std::size_t pending_offset_ = 0;

  int cancel_event_ = -1; // caller -> pump
  int stdin_event_ = -1;  // caller -> pump
  int wake_event_ = -1;   // pump -> caller

  bool reaped_ = false;
  bool killed_by_us_ = false;
  std::atomic<bool> stdin_closed_{false};
  std::atomic<bool> pump_finished_{false};
};

bool ProcessSession::Impl::Open(const CommandSpec& spec, std::string* error) {
  // O_CLOEXEC keeps these out of the child, so nothing it forks can hold the session's wakeups
  // open.
  cancel_event_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  stdin_event_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  wake_event_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (cancel_event_ < 0 || stdin_event_ < 0 || wake_event_ < 0) {
    *error = std::string("eventfd failed: ") + std::strerror(errno);
    return false;
  }

  // Done before the spawn, so the child inherits it too: a provider whose own output pipe is closed
  // is better off raising an error in its own language than dying on a signal.
  detail::EnsureSigpipeIgnored();

  const auto cmd = detail::BuildCmd(spec);
  const auto env = detail::BuildEnvMap(spec);
  try {
    // The session leader is what makes cancel reach the helpers a provider script forks: the child
    // becomes a process group leader, and the pump signals that group rather than the single pid.
    proc_.emplace(cmd, subprocess::input{subprocess::PIPE}, subprocess::output{subprocess::PIPE},
                  subprocess::error{subprocess::PIPE}, subprocess::environment{env},
                  subprocess::session_leader{true});
  } catch (const std::exception& exception) {
    *error = exception.what();
    return false;
  }

  in_fd_ = proc_->input() != nullptr ? subprocess_fileno(proc_->input()) : -1;
  out_fd_ = proc_->output() != nullptr ? subprocess_fileno(proc_->output()) : -1;
  err_fd_ = proc_->error() != nullptr ? subprocess_fileno(proc_->error()) : -1;
  if (in_fd_ < 0 || out_fd_ < 0 || err_fd_ < 0) {
    *error = "child process channels are missing.";
    return false;
  }

  // poll() only announces readiness, so no descriptor may block after this point: a provider that
  // stops reading must not be able to hold the pump past its own deadlines.
  if (!detail::MakeNonBlocking(in_fd_) || !detail::MakeNonBlocking(out_fd_) ||
      !detail::MakeNonBlocking(err_fd_)) {
    *error = std::string("fcntl failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

void ProcessSession::Impl::CloseStdinChannel() {
  stdin_channel_open_ = false;
  pending_.clear();
  pending_offset_ = 0;
  in_fd_ = -1;
  if (proc_.has_value()) {
    proc_->close_input();
  }
}

void ProcessSession::Impl::CloseOutputChannel(bool is_stdout) {
  if (is_stdout) {
    stdout_channel_open_ = false;
    out_fd_ = -1;
    if (proc_.has_value()) {
      proc_->close_output();
    }
    return;
  }
  stderr_channel_open_ = false;
  err_fd_ = -1;
  if (proc_.has_value()) {
    proc_->close_error();
  }
}

// Forwards everything currently buffered on one output channel as events. The per-channel budget
// keeps a flooding stream from starving the other one, and the queued-byte cap hands a child whose
// output nobody is consuming back to the pipe's own backpressure.
detail::DrainOutcome ProcessSession::Impl::DrainOutput(int fd, ChildEventKind kind) {
  detail::DrainOutcome outcome;
  if (queued_output_bytes_.load(std::memory_order_relaxed) >= kMaxQueuedOutputBytes) {
    return outcome;
  }
  outcome = detail::DrainBudgeted(fd, detail::kReadBudgetBytes, [&](std::string_view chunk) {
    const auto bytes = std::as_bytes(std::span<const char>(chunk.data(), chunk.size()));
    ChildEvent event;
    event.kind = kind;
    event.data.assign(bytes.begin(), bytes.end());
    QueueEvent(std::move(event));
  });
  return outcome;
}

detail::DrainOutcome ProcessSession::Impl::ServiceOutputChannel(bool is_stdout) {
  const int fd = is_stdout ? out_fd_ : err_fd_;
  if (fd < 0) {
    return {};
  }
  const ChildEventKind kind = is_stdout ? ChildEventKind::StdoutChunk : ChildEventKind::StderrChunk;
  const detail::DrainOutcome outcome = DrainOutput(fd, kind);
  if (outcome.closed) {
    CloseOutputChannel(is_stdout);
  }
  return outcome;
}

void ProcessSession::Impl::Pump() {
  stdin_channel_open_ = in_fd_ >= 0;
  stdout_channel_open_ = out_fd_ >= 0;
  stderr_channel_open_ = err_fd_ >= 0;

  bool cancel_seen = false;
  bool force_kill_sent = false;
  std::optional<Clock::time_point> terminate_deadline;

  const auto revents_of = [](const std::array<pollfd, 5>& watch, int index) -> short {
    if (index < 0) {
      return 0;
    }
    return watch.at(static_cast<std::size_t>(index)).revents;
  };

  while (true) {
    // The child's exit is what ends the session; everything else either feeds it or observes it.
    if (int code = -1; TryReapProcess(proc_->pid(), &code)) {
      reaped_ = true;
      exit_code_ = code;
      if (cancel_seen) {
        // The leader is gone, so its own grace window is over - but a helper that ignored SIGTERM
        // would otherwise outlive the session it belongs to. The group is still addressable for as
        // long as one of its members holds it, which is exactly this case.
        proc_->kill(SIGKILL);
      }
      break;
    }

    if (terminate_deadline.has_value() && Clock::now() >= *terminate_deadline) {
      if (!force_kill_sent) {
        proc_->kill(SIGKILL);
        force_kill_sent = true;
        terminate_deadline = Clock::now() + kForceGrace;
      } else {
        // SIGKILL cannot be ignored, so only a child stuck in uninterruptible sleep survives it.
        // Stop waiting here and let the destructor's last-resort reap account for it.
        break;
      }
    }

    // Only start a new block once the previous one is fully written, so stdin reaches the provider
    // in the order the caller produced it.
    if (stdin_channel_open_ && pending_offset_ >= pending_.size()) {
      pending_.clear();
      pending_offset_ = 0;
      if (std::vector<std::byte> next; stdin_queue_.try_dequeue(next)) {
        pending_ = std::move(next);
      } else if (stdin_closed_.load(std::memory_order_acquire)) {
        // Every byte the caller handed over has been written, so the child sees a clean end of
        // input instead of a truncated one.
        CloseStdinChannel();
      }
    }

    std::array<pollfd, 5> watch{};
    std::size_t watch_count = 0;
    const auto watch_fd = [&](int fd, short events) -> int {
      if (fd < 0) {
        return -1;
      }
      const auto index = static_cast<int>(watch_count);
      watch.at(watch_count) = pollfd{.fd = fd, .events = events, .revents = 0};
      watch_count += 1;
      return index;
    };

    const int cancel_index = watch_fd(cancel_event_, POLLIN);
    const int stdin_index = watch_fd(stdin_event_, POLLIN);
    const int in_index =
        stdin_channel_open_ && pending_offset_ < pending_.size() ? watch_fd(in_fd_, POLLOUT) : -1;
    // At the queued-byte cap the output channels are left out of the poll set entirely: poll()
    // would otherwise report them readable for data the pump refuses to read, spinning until the
    // caller catches up.
    const bool output_pressure =
        queued_output_bytes_.load(std::memory_order_relaxed) >= kMaxQueuedOutputBytes;
    const int out_index = stdout_channel_open_ && !output_pressure ? watch_fd(out_fd_, POLLIN) : -1;
    const int err_index = stderr_channel_open_ && !output_pressure ? watch_fd(err_fd_, POLLIN) : -1;

    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(kPollSlice);
    if (terminate_deadline.has_value()) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(*terminate_deadline - Clock::now());
      wait = std::min(wait, std::max(remaining, kZeroMs));
    }

    if (watch_count == 0) {
      // Every channel is closed but the child still runs: wait out the slice instead of spinning.
      std::this_thread::sleep_for(std::max(wait, std::chrono::milliseconds{1}));
      continue;
    }

    const int poll_rc =
        ::poll(watch.data(), static_cast<nfds_t>(watch_count), static_cast<int>(wait.count()));
    if (poll_rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      VLOG_ERROR("process session: poll failed: {}", std::strerror(errno));
      break;
    }
    if (poll_rc == 0) {
      continue;
    }

    if ((revents_of(watch, stdin_index) & POLLIN) != 0) {
      (void)DrainEventFd(stdin_event_);
    }

    if (in_index >= 0) {
      const short revents = revents_of(watch, in_index);
      if ((revents & POLLOUT) != 0) {
        const ssize_t written =
            ::write(in_fd_, pending_.data() + pending_offset_, pending_.size() - pending_offset_);
        if (written > 0) {
          pending_offset_ += static_cast<std::size_t>(written);
          if (pending_offset_ >= pending_.size()) {
            pending_.clear();
            pending_offset_ = 0;
          }
        } else if (written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
          CloseStdinChannel();
        }
      } else if ((revents & detail::kInputFailureEvents) != 0) {
        CloseStdinChannel();
      }
    }

    if (out_index >= 0 && (revents_of(watch, out_index) & detail::kReadableChannelEvents) != 0) {
      (void)ServiceOutputChannel(true);
    }

    if (err_index >= 0 && (revents_of(watch, err_index) & detail::kReadableChannelEvents) != 0) {
      (void)ServiceOutputChannel(false);
    }

    // Cancellation is handled last, after this iteration's input and output have been serviced: the
    // events it publishes then describe a boundary the caller can trust - everything the provider
    // produced before the cancel has already been queued, and nothing is written after it.
    if ((revents_of(watch, cancel_index) & POLLIN) != 0) {
      (void)DrainEventFd(cancel_event_);
      if (!cancel_seen) {
        cancel_seen = true;
        killed_by_us_ = true;
        // The provider already received everything it will ever get from this session, and a queued
        // block written now would arrive after `Cancelled` had been published.
        stdin_closed_.store(true, std::memory_order_release);
        CloseStdinChannel();
        QueueEvent(ChildEvent{
            .kind = ChildEventKind::Cancelled, .data = {}, .exit_code = -1, .killed_by_us = false});
        // Signalling the group covers the helpers a provider script forked; signalling the pid
        // alone would leave them running with the session's pipes still open.
        proc_->kill(SIGTERM);
        terminate_deadline = Clock::now() + kTerminateGrace;
      }
    }
  }

  // The child is gone, but its pipes may still hold whole lines - and a helper it forked may still
  // be writing them. Keep draining, but only while there is something to read and never past the
  // deadline, so an open write end cannot keep this thread here.
  const Clock::time_point drain_deadline = Clock::now() + kPostExitDrain;
  while (stdout_channel_open_ || stderr_channel_open_) {
    // Checked first: a helper that keeps writing would otherwise find new bytes on every iteration
    // and could hold this phase open for as long as it likes.
    const Clock::time_point now = Clock::now();
    if (now >= drain_deadline) {
      break;
    }

    std::size_t drained = 0;
    if (stdout_channel_open_) {
      drained += ServiceOutputChannel(true).bytes_read;
    }
    if (stderr_channel_open_) {
      drained += ServiceOutputChannel(false).bytes_read;
    }
    if (!stdout_channel_open_ && !stderr_channel_open_) {
      break;
    }
    if (drained > 0) {
      continue;
    }

    std::array<pollfd, 2> watch{};
    std::size_t watch_count = 0;
    if (stdout_channel_open_) {
      watch.at(watch_count) = pollfd{.fd = out_fd_, .events = POLLIN, .revents = 0};
      watch_count += 1;
    }
    if (stderr_channel_open_) {
      watch.at(watch_count) = pollfd{.fd = err_fd_, .events = POLLIN, .revents = 0};
      watch_count += 1;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(drain_deadline - now);
    const int wait =
        static_cast<int>(std::min(std::max(remaining, kZeroMs),
                                  std::chrono::duration_cast<std::chrono::milliseconds>(kPollSlice))
                             .count());
    const int poll_rc = ::poll(watch.data(), static_cast<nfds_t>(watch_count), wait);
    if (poll_rc < 0 && errno != EINTR) {
      break;
    }
  }

  // Last probe: the child may have exited while the pipes were still being drained.
  if (!reaped_) {
    if (int code = -1; TryReapProcess(proc_->pid(), &code)) {
      reaped_ = true;
      exit_code_ = code;
    }
  }

  if (stdin_channel_open_) {
    CloseStdinChannel();
  }
  if (stdout_channel_open_) {
    CloseOutputChannel(true);
  }
  if (stderr_channel_open_) {
    CloseOutputChannel(false);
  }

  QueueEvent(ChildEvent{.kind = ChildEventKind::Exited,
                        .data = {},
                        .exit_code = reaped_ ? exit_code_ : -1,
                        .killed_by_us = killed_by_us_});
  pump_finished_.store(true, std::memory_order_release);
  SignalEventFd(wake_event_);
}

void ProcessSession::Impl::QueueEvent(ChildEvent&& event) {
  const bool is_output =
      event.kind == ChildEventKind::StdoutChunk || event.kind == ChildEventKind::StderrChunk;
  const std::size_t size = event.data.size();
  // `enqueue` (not `try_enqueue`) is the allocating one: with a fixed capacity a provider that
  // prints many small lines while the caller is busy would silently lose the overflow, including
  // the terminal event, and a byte counter that had already counted the dropped chunks would keep
  // the output channels out of the poll set for good. Only a failed allocation lands below.
  if (!event_queue_.enqueue(std::move(event))) {
    VLOG_ERROR("process session: failed to queue child event");
    return;
  }
  if (is_output) {
    queued_output_bytes_.fetch_add(size, std::memory_order_relaxed);
  }
  SignalEventFd(wake_event_);
}

bool ProcessSession::Impl::PopEvent(ChildEvent& out) {
  if (!event_queue_.try_dequeue(out)) {
    return false;
  }
  if (out.kind == ChildEventKind::StdoutChunk || out.kind == ChildEventKind::StderrChunk) {
    queued_output_bytes_.fetch_sub(out.data.size(), std::memory_order_relaxed);
  }
  return true;
}

bool ProcessSession::Impl::PushStdin(std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return true;
  }
  if (pump_finished_.load(std::memory_order_acquire) ||
      stdin_closed_.load(std::memory_order_acquire)) {
    return false;
  }

  std::vector<std::byte> block(bytes.begin(), bytes.end());
  if (!stdin_queue_.try_enqueue(std::move(block))) {
    return false;
  }
  SignalEventFd(stdin_event_);
  return true;
}

void ProcessSession::Impl::CloseStdin() {
  stdin_closed_.store(true, std::memory_order_release);
  SignalEventFd(stdin_event_);
}

bool ProcessSession::Impl::NextEvent(ChildEvent& out, std::chrono::milliseconds wait) {
  if (wait <= kZeroMs) {
    return PopEvent(out);
  }

  const Clock::time_point deadline = Clock::now() + wait;
  while (true) {
    if (PopEvent(out)) {
      return true;
    }
    // Checked after the queue is drained: the pump publishes the terminal event before it reports
    // itself finished, so an event still sitting here is never mistaken for "nothing left".
    if (pump_finished_.load(std::memory_order_acquire)) {
      return false;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (remaining <= kZeroMs) {
      return false;
    }

    auto wake = pollfd{.fd = wake_event_, .events = POLLIN, .revents = 0};
    const auto wait_ms =
        std::min(remaining, std::chrono::duration_cast<std::chrono::milliseconds>(kPollSlice));
    const int poll_rc = ::poll(&wake, 1, static_cast<int>(wait_ms.count()));
    if (poll_rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (poll_rc > 0) {
      (void)DrainEventFd(wake_event_);
    }
  }
}

void ProcessSession::Impl::Cancel() const noexcept {
  if (cancel_event_ >= 0) {
    SignalEventFd(cancel_event_);
  }
}

void ProcessSession::Impl::Shutdown() noexcept {
  // Ask the pump to stop, then wait for it: it owns the child, the pipes and the escalation, so
  // nothing else may touch them while it runs. Every wait inside the pump is bounded, so this join
  // cannot outlast the force window.
  if (cancel_event_ >= 0) {
    SignalEventFd(cancel_event_);
  }
  if (pump_.joinable()) {
    pump_.join();
  }

  // Pipes are released through the streams that own them; closing the raw descriptors here would
  // close them a second time.
  if (proc_.has_value()) {
    proc_->close_input();
    proc_->close_output();
    proc_->close_error();
  }
  in_fd_ = -1;
  out_fd_ = -1;
  err_fd_ = -1;

  const auto close_if_open = [](int* fd) {
    if (*fd >= 0) {
      (void)::close(*fd);
      *fd = -1;
    }
  };
  close_if_open(&cancel_event_);
  close_if_open(&stdin_event_);
  close_if_open(&wake_event_);

  // The pump reaps as soon as the child is gone, so arriving here without a reap means the child
  // survived the pump's escalation, or never ran a pump at all - Start() can fail after the child
  // was already spawned.
  if (proc_.has_value() && !reaped_) {
    // The whole group is signalled, not just the pid: a child that had already forked helpers would
    // otherwise leave them behind. The reap that follows is bounded, because a child stuck in
    // uninterruptible sleep must not freeze destruction for good - an unreaped pid that the kernel
    // will reclaim is the lesser evil, and it is reported.
    proc_->kill(SIGKILL);
    if (int code = -1; !TryReapProcess(proc_->pid(), &code)) {
      TerminateProcessBounded(proc_->pid(), kZeroMs, kForceGrace);
      if (int unreaped_code = -1; !TryReapProcess(proc_->pid(), &unreaped_code)) {
        VLOG_ERROR("process session: child {} survived SIGKILL and stays unreaped", proc_->pid());
      }
    }
    reaped_ = true;
  }
}

ProcessSession::ProcessSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ProcessSession::ProcessSession(ProcessSession&&) noexcept = default;

ProcessSession::~ProcessSession() = default;

Expected<ProcessSession> ProcessSession::Start(const CommandSpec& spec) {
  if (spec.command.empty()) {
    return UnexpectedError("Command is empty.");
  }

  auto impl = std::make_unique<Impl>();
  if (std::string error; !impl->Open(spec, &error)) {
    return UnexpectedError(std::move(error));
  }
  impl->StartPump();
  return ProcessSession(std::move(impl));
}

bool ProcessSession::PushStdin(std::span<const std::byte> bytes) {
  return impl_->PushStdin(bytes);
}

void ProcessSession::CloseStdin() {
  impl_->CloseStdin();
}

bool ProcessSession::NextEvent(ChildEvent& out, std::chrono::milliseconds wait) {
  return impl_->NextEvent(out, wait);
}

void ProcessSession::Cancel() noexcept {
  if (impl_) {
    impl_->Cancel();
  }
}

int ProcessSession::Pid() const noexcept {
  return impl_ ? impl_->Pid() : -1;
}

} // namespace vinput::process
