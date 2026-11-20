#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string_view>
#include <thread>

namespace vinput::daemon::audio {

enum class DuckerAction : std::uint8_t { Duck, Restore };

struct DuckerTask {
  DuckerAction action{DuckerAction::Duck};
  double scale{1.0};
};

// Abstract volume control adapter allowing production wpctl invocation or in-memory testing fake.
class VolumeControlAdapter {
public:
  virtual ~VolumeControlAdapter() = default;
  virtual std::optional<double> GetVolume() = 0;
  virtual bool SetVolume(double volume) = 0;
};

// Production volume controller executing WirePlumber wpctl commands with timeouts.
class WpctlVolumeAdapter : public VolumeControlAdapter {
public:
  WpctlVolumeAdapter() = default;
  std::optional<double> GetVolume() override;
  bool SetVolume(double volume) override;

  static std::optional<double> ParseVolume(std::string_view text);
};

// In-memory fake volume controller for testing without host sound server or wpctl binary.
class FakeVolumeAdapter : public VolumeControlAdapter {
public:
  explicit FakeVolumeAdapter(double initial_volume = 1.0) : volume_(initial_volume) {}

  std::optional<double> GetVolume() override {
    const std::scoped_lock lock(mutex_);
    return volume_;
  }

  bool SetVolume(double volume) override {
    const std::scoped_lock lock(mutex_);
    volume_ = volume;
    return true;
  }

  [[nodiscard]] double CurrentVolume() const {
    const std::scoped_lock lock(mutex_);
    return volume_;
  }

private:
  mutable std::mutex mutex_;
  double volume_{1.0};
};

// Temporarily lowers the audio sink volume while recording is active, then restores it.
// Operates asynchronously via background worker tasks dispatched on-demand.
// Thread starts only when tasks are queued and terminates cleanly when queue is empty,
// eliminating 24/7 background idle thread overhead.
class OutputDucker {
public:
  explicit OutputDucker(std::shared_ptr<VolumeControlAdapter> adapter = nullptr);
  ~OutputDucker();

  OutputDucker(const OutputDucker&) = delete;
  OutputDucker& operator=(const OutputDucker&) = delete;
  OutputDucker(OutputDucker&&) = delete;
  OutputDucker& operator=(OutputDucker&&) = delete;

  // Asynchronously lowers the sink volume to (current_volume * scale).
  // scale is clamped to [0.0, 1.0]. Idempotent: a second call while already ducked is a no-op.
  void Duck(double scale);

  // Asynchronously restores the volume saved by the most recent Duck(). No-op if not ducked.
  void Restore();

private:
  void EnsureWorkerRunningLocked();
  void StopAndJoin();
  void WorkerLoop();
  void ProcessDuck(double scale);
  void ProcessRestore();

  std::shared_ptr<VolumeControlAdapter> adapter_;
  mutable std::mutex mutex_;
  std::queue<DuckerTask> queue_;
  bool stop_{false};
  bool logical_ducked_{false};
  bool worker_ducked_{false};
  bool worker_running_{false};
  double saved_volume_{0.0};
  std::thread worker_;
};

} // namespace vinput::daemon::audio
