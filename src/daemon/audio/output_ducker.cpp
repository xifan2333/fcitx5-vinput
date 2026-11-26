#include "daemon/audio/output_ducker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/volume.h>
#include <string>
#include <thread>
#include <utility>

#include "common/audio/pulse_raii.h"
#include "common/utils/debug_log.h"

namespace vinput::daemon::audio {

namespace {

constexpr const char* kDefaultSinkName = "@DEFAULT_SINK@";
constexpr int kPulseVolumeTimeoutMs = 1500;
constexpr double kMaxVolumeScalar = 1.5;

struct SinkQueryResult {
  std::optional<pa_cvolume> volume;
  std::string sink_name;
};

void OnSinkInfo(pa_context* /*ctx*/, const pa_sink_info* info, int eol, void* userdata) noexcept {
  if (eol != 0 || info == nullptr || userdata == nullptr) {
    return;
  }
  auto* out = static_cast<SinkQueryResult*>(userdata);
  if (pa_cvolume_valid(&info->volume) != 0 && info->volume.channels > 0 && info->name != nullptr &&
      info->name[0] != '\0') {
    try {
      out->sink_name = info->name;
      out->volume = info->volume;
    } catch (...) {
      out->volume.reset();
      out->sink_name.clear();
    }
  }
}

void OnContextSuccess(pa_context* /*ctx*/, int success, void* userdata) noexcept {
  if (userdata != nullptr) {
    *static_cast<bool*>(userdata) = (success != 0);
  }
}

std::optional<SinkQueryResult>
QueryDefaultSinkInfo(vinput::pulse::PulseSyncSession& session,
                     std::chrono::steady_clock::time_point deadline) {
  SinkQueryResult result;
  if (!session.RunOperation(pa_context_get_sink_info_by_name(session.Context(), kDefaultSinkName,
                                                             &OnSinkInfo, &result),
                            deadline)) {
    return std::nullopt;
  }
  if (!result.volume.has_value() || result.sink_name.empty()) {
    return std::nullopt;
  }
  return result;
}

} // namespace

std::optional<double> PulseVolumeAdapter::CvolumeToScalar(const pa_cvolume& cvol) {
  if (pa_cvolume_valid(&cvol) == 0 || cvol.channels == 0) {
    return std::nullopt;
  }
  return static_cast<double>(pa_cvolume_max(&cvol)) / static_cast<double>(PA_VOLUME_NORM);
}

pa_cvolume PulseVolumeAdapter::ScaleCvolume(const pa_cvolume& baseline, double target_scalar) {
  const double clamped = std::clamp(target_scalar, 0.0, kMaxVolumeScalar);
  const auto target_raw =
      static_cast<pa_volume_t>(std::lround(clamped * static_cast<double>(PA_VOLUME_NORM)));

  pa_cvolume result = baseline;
  if (pa_cvolume_valid(&result) == 0 || result.channels == 0) {
    pa_cvolume_set(&result, 2, target_raw);
    return result;
  }
  if (pa_cvolume_max(&result) == PA_VOLUME_MUTED) {
    pa_cvolume_set(&result, result.channels, target_raw);
    return result;
  }
  if (pa_cvolume_scale(&result, target_raw) == nullptr) {
    pa_cvolume_set(&result, result.channels, target_raw);
  }
  return result;
}

std::optional<double> PulseVolumeAdapter::GetVolume() {
  {
    const std::scoped_lock lock(state_mutex_);
    baseline_cvolume_.reset();
    baseline_scalar_ = 0.0;
    baseline_sink_name_.clear();
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kPulseVolumeTimeoutMs);
  vinput::pulse::PulseSyncSession session;
  if (!session.Connect("vinput-ducker", deadline)) {
    return std::nullopt;
  }

  auto info = QueryDefaultSinkInfo(session, deadline);
  if (!info || !info->volume) {
    return std::nullopt;
  }
  const auto scalar = CvolumeToScalar(*info->volume);
  if (!scalar) {
    return std::nullopt;
  }

  {
    const std::scoped_lock lock(state_mutex_);
    baseline_cvolume_ = *info->volume;
    baseline_scalar_ = *scalar;
    baseline_sink_name_ = std::move(info->sink_name);
  }
  return scalar;
}

bool PulseVolumeAdapter::SetVolume(double volume) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kPulseVolumeTimeoutMs);
  vinput::pulse::PulseSyncSession session;
  if (!session.Connect("vinput-ducker", deadline)) {
    return false;
  }

  std::optional<pa_cvolume> base;
  double base_scalar = 0.0;
  std::string target_sink;
  {
    const std::scoped_lock lock(state_mutex_);
    base = baseline_cvolume_;
    base_scalar = baseline_scalar_;
    target_sink = baseline_sink_name_;
  }

  if (!base || target_sink.empty()) {
    const auto info = QueryDefaultSinkInfo(session, deadline);
    if (!info || !info->volume) {
      return false;
    }
    base = info->volume;
    base_scalar = CvolumeToScalar(*base).value_or(0.0);
    target_sink = info->sink_name;
  }

  const pa_cvolume target_cvol =
      (std::abs(volume - base_scalar) < 1e-6) ? *base : ScaleCvolume(*base, volume);

  bool ok = false;
  if (!session.RunOperation(pa_context_set_sink_volume_by_name(session.Context(),
                                                               target_sink.c_str(), &target_cvol,
                                                               &OnContextSuccess, &ok),
                            deadline)) {
    return false;
  }
  return ok;
}

OutputDucker::OutputDucker(std::shared_ptr<VolumeControlAdapter> adapter)
    : adapter_(adapter != nullptr ? std::move(adapter) : std::make_shared<PulseVolumeAdapter>()) {}

OutputDucker::~OutputDucker() {
  StopAndJoin();
  if (worker_ducked_) {
    ProcessRestore();
  }
}

void OutputDucker::EnsureWorkerRunningLocked() {
  if (!worker_running_) {
    if (worker_.joinable()) {
      worker_.join();
    }
    worker_running_ = true;
    worker_ = std::thread([this]() { WorkerLoop(); });
  }
}

void OutputDucker::StopAndJoin() {
  {
    const std::scoped_lock lock(mutex_);
    stop_ = true;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

void OutputDucker::Duck(double scale) {
  const std::scoped_lock lock(mutex_);
  if (stop_ || logical_ducked_) {
    return;
  }
  logical_ducked_ = true;
  queue_.push(DuckerTask{.action = DuckerAction::Duck, .scale = scale});
  EnsureWorkerRunningLocked();
}

void OutputDucker::Restore() {
  const std::scoped_lock lock(mutex_);
  if (stop_ || !logical_ducked_) {
    return;
  }
  logical_ducked_ = false;
  queue_.push(DuckerTask{.action = DuckerAction::Restore, .scale = 1.0});
  EnsureWorkerRunningLocked();
}

void OutputDucker::WorkerLoop() {
  while (true) {
    DuckerTask task;
    {
      const std::scoped_lock lock(mutex_);
      if (stop_ || queue_.empty()) {
        worker_running_ = false;
        return;
      }
      task = queue_.front();
      queue_.pop();
    }

    if (task.action == DuckerAction::Duck) {
      ProcessDuck(task.scale);
    } else {
      ProcessRestore();
    }
  }
}

void OutputDucker::ProcessDuck(double scale) {
  if (worker_ducked_ || !adapter_) {
    return;
  }
  scale = std::clamp(scale, 0.0, 1.0);

  const auto current = adapter_->GetVolume();
  if (!current) {
    VLOG_WARN("output ducking skipped (volume adapter unavailable or default sink not found)");
    return;
  }

  const double target = *current * scale;
  if (!adapter_->SetVolume(target)) {
    VLOG_WARN("set-volume call failed or timed out; assuming volume was applied");
    saved_volume_ = *current;
    worker_ducked_ = true;
    return;
  }

  saved_volume_ = *current;
  worker_ducked_ = true;
  VLOG_DEBUG("ducked output volume {:.2f} -> {:.2f} ({:.0f}%)", *current, target, scale * 100.0);
}

void OutputDucker::ProcessRestore() {
  if (!worker_ducked_ || !adapter_) {
    return;
  }
  worker_ducked_ = false;
  const double restore_to = saved_volume_;
  saved_volume_ = 0.0;
  if (adapter_->SetVolume(restore_to)) {
    VLOG_DEBUG("restored output volume -> {:.2f}", restore_to);
  } else {
    VLOG_WARN("failed to restore output volume to {:.2f}", restore_to);
  }
}

} // namespace vinput::daemon::audio
