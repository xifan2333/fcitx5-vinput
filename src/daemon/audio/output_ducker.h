#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <pulse/volume.h>
#include <queue>
#include <thread>

namespace vinput::daemon::audio {

enum class DuckerAction : std::uint8_t { Duck, Restore };

struct DuckerTask {
  DuckerAction action{DuckerAction::Duck};
  double scale{1.0};
};

// Abstract volume control adapter allowing production libpulse control or in-memory testing fake.
class VolumeControlAdapter {
public:
  VolumeControlAdapter() = default;
  virtual ~VolumeControlAdapter() = default;
  VolumeControlAdapter(const VolumeControlAdapter&) = default;
  VolumeControlAdapter& operator=(const VolumeControlAdapter&) = default;
  VolumeControlAdapter(VolumeControlAdapter&&) = default;
  VolumeControlAdapter& operator=(VolumeControlAdapter&&) = delete;

  virtual std::optional<double> GetVolume() = 0;
  virtual bool SetVolume(double volume) = 0;
};

// Production volume controller reading/writing @DEFAULT_SINK@ volume via libpulse with timeouts.
class PulseVolumeAdapter : public VolumeControlAdapter {
public:
  PulseVolumeAdapter() = default;
  std::optional<double> GetVolume() override;
  bool SetVolume(double volume) override;

  static std::optional<double> CvolumeToScalar(const pa_cvolume& cvol);
  static pa_cvolume ScaleCvolume(const pa_cvolume& baseline, double target_scalar);

private:
  std::mutex state_mutex_;
  std::optional<pa_cvolume> baseline_cvolume_;
  double baseline_scalar_{0.0};
};

// In-memory fake volume controller for testing without host sound server.
class FakeVolumeAdapter : public VolumeControlAdapter {
public:
  explicit FakeVolumeAdapter(double initial_volume = 1.0) : volume_(initial_volume) {}

  std::optional<double> GetVolume() override {
    const std::scoped_lock lock(mutex_);
    if (fail_get_) {
      return std::nullopt;
    }
    return volume_;
  }

  bool SetVolume(double volume) override {
    const std::scoped_lock lock(mutex_);
    if (fail_set_) {
      return false;
    }
    volume_ = volume;
    ++write_count_;
    return true;
  }

  [[nodiscard]] double CurrentVolume() const {
    const std::scoped_lock lock(mutex_);
    return volume_;
  }

  [[nodiscard]] int WriteCount() const {
    const std::scoped_lock lock(mutex_);
    return write_count_;
  }

  void SetFailGet(bool fail) {
    const std::scoped_lock lock(mutex_);
    fail_get_ = fail;
  }

  void SetFailSet(bool fail) {
    const std::scoped_lock lock(mutex_);
    fail_set_ = fail;
  }

private:
  mutable std::mutex mutex_;
  double volume_{1.0};
  int write_count_{0};
  bool fail_get_{false};
  bool fail_set_{false};
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
