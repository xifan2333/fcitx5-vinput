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

// Resolves a target string (e.g. "probe.monitor" -> node "probe", sink capture = true).
ResolvedCaptureTarget ResolveCaptureTarget(std::string_view target);

std::vector<DeviceInfo> EnumerateAudioSources();

} // namespace vinput::pw
