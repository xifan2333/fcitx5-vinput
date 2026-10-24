#include "daemon/audio/output_ducker.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "common/utils/debug_log.h"
#include "common/utils/process_utils.h"

namespace vinput::daemon::audio {

namespace {

constexpr char kWpctl[] = "wpctl";
constexpr char kDefaultSink[] = "@DEFAULT_AUDIO_SINK@";
constexpr std::string_view kVolumePrefix = "Volume:";

// Parse the numeric volume out of `wpctl get-volume` output, e.g.
// "Volume: 1.00" or "Volume: 0.15 [MUTED]".
std::optional<double> ParseVolume(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  // Locate the marker, then let it own its own length: no hand-counted offsets that go stale if
  // wpctl ever renames the field.
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

// wpctl is a local process that must answer almost instantly. A deadline keeps the ducking worker
// thread from blocking forever if WirePlumber hangs: a timed-out or failed call is treated the
// same as "wpctl unavailable" so recording is never stalled by volume control.
constexpr int kWpctlTimeoutMs = 2000;

// Reads the current volume of the default sink through the unified process entry, which owns the
// deadline, the session leader and reaping.
std::optional<double> ReadDefaultSinkVolume() {
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

// Sets the volume of the default sink through the unified process entry. Only the exit code
// matters, so every non-success outcome (launch failure, timeout, non-zero exit) collapses to
// false and the caller skips the ducking step.
bool SetDefaultSinkVolume(double volume) {
  std::array<char, 32> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%.4F", volume);
  if (written <= 0) {
    return false;
  }
  if (static_cast<std::size_t>(written) >= buffer.size()) {
    return false;
  }

  vinput::process::CommandSpec spec;
  spec.command = kWpctl;
  spec.args = {"set-volume", kDefaultSink, std::string(buffer.data(), buffer.data() + written)};
  spec.timeout_ms = kWpctlTimeoutMs;

  const auto result = vinput::process::Run(spec);
  return !result.launch_failed && !result.timed_out && result.exit_code == 0;
}

} // namespace

OutputDucker::OutputDucker() {
  worker_ = std::thread([this]() { WorkerLoop(); });
}

OutputDucker::~OutputDucker() {
  {
    const std::scoped_lock lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  if (worker_ducked_) {
    ProcessRestore();
  }
}

void OutputDucker::Duck(double scale) {
  {
    const std::scoped_lock lock(mutex_);
    if (logical_ducked_) {
      return;
    }
    logical_ducked_ = true;
    queue_.push(DuckerTask{.action = DuckerAction::Duck, .scale = scale});
  }
  cv_.notify_one();
}

void OutputDucker::Restore() {
  {
    const std::scoped_lock lock(mutex_);
    if (!logical_ducked_) {
      return;
    }
    logical_ducked_ = false;
    queue_.push(DuckerTask{.action = DuckerAction::Restore, .scale = 1.0});
  }
  cv_.notify_one();
}

void OutputDucker::Flush() {
  std::unique_lock lock(mutex_);
  cv_flush_.wait(lock, [this]() { return queue_.empty(); });
}

bool OutputDucker::IsDucked() const {
  const std::scoped_lock lock(mutex_);
  return logical_ducked_;
}

void OutputDucker::WorkerLoop() {
  while (true) {
    DuckerTask task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) {
        break;
      }
      task = queue_.front();
      queue_.pop();
    }

    if (task.action == DuckerAction::Duck) {
      ProcessDuck(task.scale);
    } else {
      ProcessRestore();
    }

    {
      const std::scoped_lock lock(mutex_);
      if (queue_.empty()) {
        cv_flush_.notify_all();
      }
    }
  }
}

void OutputDucker::ProcessDuck(double scale) {
  if (worker_ducked_) {
    return;
  }
  scale = std::clamp(scale, 0.0, 1.0);

  const auto current = ReadDefaultSinkVolume();
  if (!current) {
    VLOG_WARN("output ducking skipped (wpctl unavailable or no default sink)");
    return;
  }

  const double target = *current * scale;
  if (!SetDefaultSinkVolume(target)) {
    // wpctl is fire-and-forget: the volume may have been applied even though the call timed out.
    // Since we know the volume before the attempt, assume the write took effect and mark the
    // worker ducked so Restore() can try to undo it. This is safer than leaving the volume
    // lowered with no way to restore it.
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
  if (!worker_ducked_) {
    return;
  }
  worker_ducked_ = false;
  const double restore_to = saved_volume_;
  saved_volume_ = 0.0;
  if (SetDefaultSinkVolume(restore_to)) {
    VLOG_DEBUG("restored output volume -> {:.2f}", restore_to);
  } else {
    VLOG_WARN("failed to restore output volume to {:.2f}", restore_to);
  }
}

} // namespace vinput::daemon::audio
