#pragma once

#include <cstdint>
#include <optional>
#include <spa/utils/dict.h>
#include <string>
#include <string_view>
#include <vector>

namespace vinput::pw {

struct DeviceInfo {
  uint32_t id{0};
  std::string name;
  std::string description;
  bool is_sink_monitor{false};
  std::string object_serial;
};

struct ResolvedCaptureTarget {
  std::string node_name;
  bool is_sink_capture{false};
  std::string object_serial;
};

// Parses WirePlumber's "default" metadata JSON payload (e.g. `{"name":"..."}`).
std::string ParseDefaultAudioSourceMetadata(std::string_view json);

// Parses a registry node global event into DeviceInfo if it is Audio/Source or Audio/Sink.
std::optional<DeviceInfo> ParseRegistryNodeDevice(uint32_t id, const spa_dict* props);

// Resolves a target string (e.g. "probe.monitor" -> node "probe", sink capture = true).
// If known_devices is provided, matches against actual device metadata (propagating object_serial)
// before fallback.
ResolvedCaptureTarget ResolveCaptureTarget(std::string_view target,
                                           const std::vector<DeviceInfo>& known_devices = {});

// Resolves a runtime capture target using cached devices and live default source metadata.
// Empty/"default" resolves via default_source_name when found in known_devices, otherwise returns
// an empty target so callers omit PW_KEY_TARGET_OBJECT (never passing literal "default").
ResolvedCaptureTarget ResolveLiveCaptureTarget(std::string_view target,
                                               const std::vector<DeviceInfo>& known_devices,
                                               std::string_view default_source_name);

std::vector<DeviceInfo> EnumerateAudioSources();

} // namespace vinput::pw
