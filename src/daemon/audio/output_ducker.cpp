#include "daemon/audio/output_ducker.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "common/utils/debug_log.h"
#include "common/utils/process_utils.h"

namespace vinput::daemon::audio {

namespace {

constexpr char kWpctl[] = "wpctl";
constexpr char kDefaultSink[] = "@DEFAULT_AUDIO_SINK@";
constexpr std::string_view kVolumePrefix = "Volume:";
constexpr int kWpctlTimeoutMs = 2000;

} // namespace

std::optional<double> WpctlVolumeAdapter::ParseVolume(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  const auto prefix_pos = text.find(kVolumePrefix);
  if (prefix_pos == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view after_prefix = text.substr(prefix_pos + kVolumePrefix.size());
  const auto num_start = after_prefix.find_first_not_of(" \t");
  if (num_start == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view number = after_prefix.substr(num_start);
  double value = 0.0;
  const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
  if (parsed.ec != std::errc{}) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> WpctlVolumeAdapter::GetVolume() {
  vinput::process::CommandSpec spec;
  spec.command = kWpctl;
  spec.args = {"get-volume", kDefaultSink};
  spec.timeout_ms = kWpctlTimeoutMs;

  const auto result = vinput::process::Run(spec);
  if (result.launch_failed || result.timed_out || result.exit_code != 0) {
    return std::nullopt;
  }
  return ParseVolume(result.stdout_text);
}

bool WpctlVolumeAdapter::SetVolume(double volume) {
  std::array<char, 32> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%.4F", volume);
  if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size()) {
    return false;
  }

  vinput::process::CommandSpec spec;
  spec.command = kWpctl;
  spec.args = {"set-volume", kDefaultSink, std::string(buffer.data(), buffer.data() + written)};
  spec.timeout_ms = kWpctlTimeoutMs;

  const auto result = vinput::process::Run(spec);
  return !result.launch_failed && !result.timed_out && result.exit_code == 0;
}

OutputDucker::OutputDucker(std::shared_ptr<VolumeControlAdapter> adapter)
    : adapter_(adapter != nullptr ? std::move(adapter) : std::make_shared<WpctlVolumeAdapter>()) {}

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
