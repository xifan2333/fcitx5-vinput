#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "common/utils/expected.h"
#include "common/utils/process_utils.h"

namespace vinput::process {

// What a `ProcessSession` reports while its child runs.
enum class ChildEventKind : std::uint8_t {
  // Raw bytes the child printed. Framing and protocol semantics stay with the caller.
  StdoutChunk,
  StderrChunk,
  // The caller asked to stop the session; nothing more will be written to the child, and the pump
  // is terminating its process group. Always followed by `Exited`.
  Cancelled,
  // Terminal event: the child is gone and reaped.
  Exited,
};

struct ChildEvent {
  ChildEventKind kind = ChildEventKind::Exited;
  // `StdoutChunk` / `StderrChunk` payload.
  std::vector<std::byte> data;
  // `Exited` only: the child's status as 0-255, or 128+signal when a signal ended it.
  int exit_code = -1;
  // `Exited` only: the child died because this session asked it to, which tells a cancellation
  // apart from a provider that crashed on its own.
  bool killed_by_us = false;
};

// Direct stream sinks for child output. When provided to `Start`, raw bytes bypass the event
// queue and are dispatched to the sink immediately on the pump thread.
using StderrSink = std::function<void(std::span<const std::byte>)>;
using StdoutSink = std::function<void(std::span<const std::byte>)>;

// A child process that outlives any single call: the caller keeps feeding stdin and reading output
// while the child runs, on the child's schedule rather than the caller's.
//
// `Run` cannot serve this shape. It takes the whole input up front, returns only once the child is
// gone, and therefore binds every observation of the child to the caller's own progress. A
// streaming provider is the opposite: its stdout, its stderr and its death all arrive while the
// caller may be busy elsewhere, may be waiting for exactly one of them, or may want to stop it from
// a different thread. This class owns that shape - the pump thread holds the three pipes, the
// caller only hands over bytes and pops events - so a provider that stops reading, stops writing,
// or hangs cannot extend the caller's own deadlines.
//
// Threading contract:
//  - `PushStdin`, `CloseStdin` and `NextEvent` are called from one owner thread at a time.
//  - `Cancel` may be called from any thread, including while the owner is blocked in `NextEvent`.
//  - Every event is delivered in the order the child produced it, and `Exited` is last.
class ProcessSession {
public:
  // Starts `spec` in its own session, so cancelling reaches the helpers it forked. The spec's
  // timeout does not apply: the session lives until the child exits, the caller cancels, or the
  // caller stops draining - a long recording is not bounded by the first chunk's budget.
  // When `stderr_sink` or `stdout_sink` is provided, the pump thread delivers bytes directly to it
  // as they arrive, bypassing the event queue.
  static Expected<ProcessSession> Start(const CommandSpec& spec, StderrSink stderr_sink = nullptr,
                                        StdoutSink stdout_sink = nullptr);

  ProcessSession(ProcessSession&& other) noexcept;
  ProcessSession& operator=(ProcessSession&& other) noexcept = delete;
  ProcessSession(const ProcessSession&) = delete;
  ProcessSession& operator=(const ProcessSession&) = delete;

  // Stops the child if it still runs, reaps it, and releases every descriptor.
  ~ProcessSession();

  // Hands `bytes` to the pump thread. False means the bytes were not accepted - the session is
  // finished, or the child has stopped reading and the bounded queue is full - so the caller can
  // report backpressure instead of buffering without limit.
  bool PushStdin(std::span<const std::byte> bytes);

  // Delivers EOF after every byte already handed to `PushStdin`; the child sees a clean end of
  // input rather than a truncated one.
  void CloseStdin();

  // Pops the next event, waiting at most `wait` for one to arrive. False means nothing arrived in
  // time, or the session has nothing left to report. A zero `wait` never blocks.
  bool NextEvent(ChildEvent& out, std::chrono::milliseconds wait);

  // Asks the pump thread to terminate the child's process group. Idempotent, never blocks, and safe
  // from any thread: it only signals the pump, which owns the child.
  void Cancel() noexcept;

  // True when the child process has exited and been reaped.
  [[nodiscard]] bool HasExited(int* exit_code = nullptr) const noexcept;

  // Waits up to `wait` for the child process to exit. Returns true if exited within the window.
  bool WaitForExit(std::chrono::milliseconds wait, int* exit_code = nullptr);

  // The child's pid. Diagnostics only: the session owns the child, and the pid stops being a valid
  // handle the moment the session reaps it.
  [[nodiscard]] int Pid() const noexcept;

private:
  struct Impl;

  explicit ProcessSession(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace vinput::process
