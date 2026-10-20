#include "daemon/audio/output_ducker.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <subprocess.hpp>
#include <vector>

#include "common/utils/debug_log.h"

namespace vinput::daemon::audio {

namespace {

constexpr char kWpctl[] = "wpctl";
constexpr char kDefaultSink[] = "@DEFAULT_AUDIO_SINK@";

// Parse the numeric volume out of `wpctl get-volume` output, e.g.
// "Volume: 1.00" or "Volume: 0.15 [MUTED]".
std::optional<double> ParseVolume(const std::string& text) {
  const auto pos = text.find("Volume:");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  const char* cursor = text.c_str() + pos + 7; // skip "Volume:"
  char* end = nullptr;
  const double value = std::strtod(cursor, &end);
  if (end == cursor) {
    return std::nullopt;
  }
  return value;
}

// Reads the current volume of the default sink using a single stdout pipe
// without allocating stdin or stderr pipes.
std::optional<double> ReadDefaultSinkVolume() {
  try {
    auto proc =
        subprocess::Popen({kWpctl, "get-volume", kDefaultSink},
                          subprocess::output{subprocess::PIPE}, subprocess::error{"/dev/null"});
    auto [out_buf, err_buf] = proc.communicate();
    if (proc.retcode() != 0) {
      return std::nullopt;
    }
    return ParseVolume(std::string(out_buf.buf.data(), out_buf.length));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// Sets the volume of the default sink using zero pipes (redirecting streams
// to /dev/null), eliminating pipe leaks and buffer contention.
bool SetDefaultSinkVolume(double volume) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.4F", volume);

  try {
    return subprocess::call({kWpctl, "set-volume", kDefaultSink, buffer},
                            subprocess::input{"/dev/null"}, subprocess::output{"/dev/null"},
                            subprocess::error{"/dev/null"}) == 0;
  } catch (const std::exception&) {
    return false;
  }
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
    queue_.push(DuckerTask{DuckerAction::Duck, scale});
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
    queue_.push(DuckerTask{DuckerAction::Restore, 1.0});
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
    fprintf(stderr,
            "vinput-daemon: output ducking skipped (wpctl unavailable or no default sink)\n");
    return;
  }

  const double target = *current * scale;
  if (!SetDefaultSinkVolume(target)) {
    fprintf(stderr, "vinput-daemon: output ducking skipped (failed to set volume)\n");
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
    fprintf(stderr, "vinput-daemon: failed to restore output volume to %.2f\n", restore_to);
  }
}

} // namespace vinput::daemon::audio
