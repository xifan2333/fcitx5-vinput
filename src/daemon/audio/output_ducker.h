#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>

namespace vinput::daemon::audio {

enum class DuckerAction : std::uint8_t { Duck, Restore };

struct DuckerTask {
  DuckerAction action{DuckerAction::Duck};
  double scale{1.0};
};

// Temporarily lowers the default audio sink volume while recording is active,
// then restores it afterwards. Volume is driven through WirePlumber (wpctl) so
// that it matches the system volume the user sees and hears, stays consistent
// with the session manager, and is fully restorable.
//
// Duck() and Restore() are asynchronous and return immediately without
// blocking the caller or D-Bus main thread. Tasks are executed in FIFO order
// on a background worker thread. All operations are best-effort: if wpctl is
// unavailable or fails, ducking is skipped so recording is never blocked.
class OutputDucker {
public:
  OutputDucker();
  ~OutputDucker();

  OutputDucker(const OutputDucker&) = delete;
  OutputDucker& operator=(const OutputDucker&) = delete;

  // Asynchronously lowers the default sink to (current_volume * scale).
  // scale is clamped to [0.0, 1.0]. Idempotent: a second call while already ducked is a no-op.
  void Duck(double scale);

  // Asynchronously restores the volume saved by the most recent Duck(). No-op if not ducked.
  void Restore();

  // Waits for all pending ducking/restore tasks to finish.
  void Flush();

  // Returns true if logically ducked or ducking task is active/queued.
  bool IsDucked() const;

private:
  void WorkerLoop();
  void ProcessDuck(double scale);
  void ProcessRestore();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable cv_flush_;
  std::queue<DuckerTask> queue_;
  bool stop_{false};
  bool logical_ducked_{false};
  bool worker_ducked_{false};
  double saved_volume_{0.0};
  std::thread worker_;
};

} // namespace vinput::daemon::audio
