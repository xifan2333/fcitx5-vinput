#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vinput::pw {

struct DeviceInfo {
  uint32_t id{0};
  std::string name;
  std::string description;
  bool is_sink_monitor{false};
};

struct ResolvedCaptureTarget {
  std::string node_name;
  bool is_sink_capture{false};
};

// `global.capture_device` value that selects the first source matching the
// built-in priority list below.
inline constexpr std::string_view kAutoCaptureTarget = "auto";
// Default priority order: an ATVV source first, then the system default.
inline constexpr std::string_view kAutoCapturePriority = "atvv-*,default";

// True when the target is `auto`, a comma-separated priority list, or a
// wildcard pattern. An exact device name takes precedence when resolving.
bool IsCaptureTargetPattern(std::string_view target);

// Resolves a target string (e.g. "probe.monitor" -> node "probe", sink capture = true).
// If known_devices is provided, matches against actual device metadata before fallback.
ResolvedCaptureTarget ResolveCaptureTarget(std::string_view target,
                                           const std::vector<DeviceInfo>& known_devices = {});

std::vector<DeviceInfo> EnumerateAudioSources();

} // namespace vinput::pw
