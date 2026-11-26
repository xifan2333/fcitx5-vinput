#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vinput::pulse {

struct DeviceInfo {
  uint32_t id{0};
  std::string name;
  std::string description;
  bool is_sink_monitor{false};
};

struct ResolvedCaptureTarget {
  // Empty means system default source (pass nullptr to pa_stream_connect_record).
  // Non-empty is the exact PulseAudio source name (including "<sink>.monitor" for monitor sources).
  std::string source_name;
  bool is_sink_monitor{false};
};

struct AudioServerSnapshot {
  std::string default_source_name;
  std::string default_sink_name;
  std::vector<DeviceInfo> sources;
  bool connected{false};
};

// Resolves a configured or per-trigger target string into a PulseAudio source target.
// - "" / "default" / "@DEFAULT_SOURCE@": resolves to default_source_name when provided, otherwise
//   returns an empty source_name for zero-latency server-default capture.
// - Explicit source or "<sink>.monitor": preserves the exact PulseAudio source name and determines
//   is_sink_monitor from known_devices when matched, falling back to the ".monitor" suffix rule.
ResolvedCaptureTarget ResolveCaptureTarget(std::string_view target,
                                           std::span<const DeviceInfo> known_devices = {},
                                           std::string_view default_source_name = {});

// Formats a human-readable connection error including pipewire-pulse guidance.
std::string FormatConnectionError(int pa_err);

// Queries server default source/sink and enumerates all available audio sources (mics + monitors).
AudioServerSnapshot QueryAudioServerSnapshot(int timeout_ms = 1500);

// Convenience wrapper returning QueryAudioServerSnapshot().sources.
std::vector<DeviceInfo> EnumerateAudioSources(int timeout_ms = 1500);

} // namespace vinput::pulse
