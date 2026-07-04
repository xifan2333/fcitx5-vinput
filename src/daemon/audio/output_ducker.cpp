#include "daemon/audio/output_ducker.h"

#include "common/utils/debug_log.h"
#include "common/utils/process_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace vinput::daemon::audio {

namespace {

constexpr char kWpctl[] = "wpctl";
constexpr char kDefaultSink[] = "@DEFAULT_AUDIO_SINK@";
constexpr int kWpctlTimeoutMs = 2000;

vinput::process::CommandResult RunWpctl(std::vector<std::string> args) {
  vinput::process::CommandSpec spec;
  spec.command = kWpctl;
  spec.args = std::move(args);
  spec.timeout_ms = kWpctlTimeoutMs;
  return vinput::process::RunCommandWithInput(spec, {});
}

// Parse the numeric volume out of `wpctl get-volume` output, e.g.
// "Volume: 1.00" or "Volume: 0.15 [MUTED]".
std::optional<double> ParseVolume(const std::string &text) {
  const auto pos = text.find("Volume:");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  const char *cursor = text.c_str() + pos + 7;  // skip "Volume:"
  char *end = nullptr;
  const double value = std::strtod(cursor, &end);
  if (end == cursor) {
    return std::nullopt;
  }
  return value;
}

// Reads the current volume of the default sink. std::nullopt if wpctl is
// unavailable or the output cannot be parsed.
std::optional<double> ReadDefaultSinkVolume() {
  const auto result = RunWpctl({"get-volume", kDefaultSink});
  if (result.launch_failed || result.timed_out || result.exit_code != 0) {
    return std::nullopt;
  }
  return ParseVolume(result.stdout_text);
}

bool SetDefaultSinkVolume(double volume) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.4f", volume);
  const auto result = RunWpctl({"set-volume", kDefaultSink, buffer});
  return !result.launch_failed && !result.timed_out && result.exit_code == 0;
}

}  // namespace

void OutputDucker::Duck(double scale) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ducked_) {
    return;
  }
  scale = std::clamp(scale, 0.0, 1.0);

  const auto current = ReadDefaultSinkVolume();
  if (!current) {
    fprintf(stderr,
            "vinput-daemon: output ducking skipped (wpctl unavailable or no "
            "default sink)\n");
    return;
  }

  const double target = *current * scale;
  if (!SetDefaultSinkVolume(target)) {
    fprintf(stderr, "vinput-daemon: output ducking skipped (failed to set "
                    "volume)\n");
    return;
  }

  saved_volume_ = *current;
  ducked_ = true;
  vinput::debug::Log("ducked output volume %.2f -> %.2f (%.0f%%)\n", *current,
                     target, scale * 100.0);
}

void OutputDucker::Restore() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ducked_) {
    return;
  }
  ducked_ = false;
  const double restore_to = saved_volume_;
  saved_volume_ = 0.0;
  if (SetDefaultSinkVolume(restore_to)) {
    vinput::debug::Log("restored output volume -> %.2f\n", restore_to);
  } else {
    fprintf(stderr,
            "vinput-daemon: failed to restore output volume to %.2f\n",
            restore_to);
  }
}

}  // namespace vinput::daemon::audio
