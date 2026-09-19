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

// Resolves a target string (e.g. "probe.monitor" -> node "probe", sink capture = true;
// "source:mic.monitor" -> node "mic.monitor", sink capture = false).
ResolvedCaptureTarget ResolveCaptureTarget(std::string_view target,
                                           const std::vector<DeviceInfo>& known_devices = {});

std::vector<DeviceInfo> EnumerateAudioSources();

} // namespace vinput::pw
