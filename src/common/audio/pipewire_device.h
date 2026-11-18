#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct pw_core;
struct pw_thread_loop;

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

// Persistent PipeWire control-plane watcher keeping an incremental cache of audio nodes
// (including object.serial) and the "default.audio.source" metadata key without creating streams.
class ControlPlane {
public:
  ControlPlane();
  ~ControlPlane();

  ControlPlane(const ControlPlane&) = delete;
  ControlPlane& operator=(const ControlPlane&) = delete;
  ControlPlane(ControlPlane&&) = delete;
  ControlPlane& operator=(ControlPlane&&) = delete;

  // Attaches a started pw_thread_loop and connects core/registry/metadata watchers.
  // Returns false without failing permanently if PipeWire is temporarily unavailable.
  bool Connect(pw_thread_loop* thread_loop, int sync_timeout_ms = 250);

  // Ensures the persistent connection is healthy, reconnecting after a PipeWire restart or
  // deferred startup connection. Caller must NOT hold the thread_loop lock.
  bool EnsureConnected(pw_thread_loop* thread_loop, int sync_timeout_ms = 250);

  // Tears down all control-plane proxies under the attached thread_loop lock before stopping
  // the loop. Automatically invoked by ~ControlPlane() if still attached.
  void Disconnect() noexcept;

  // Returns the persistent core handle for pw_stream_new (valid while EnsureConnected holds
  // under thread_loop lock), or nullptr if disconnected.
  [[nodiscard]] pw_core* Core() const noexcept;

  // True if the control-plane connection has observed a fatal core/remote disconnect error.
  [[nodiscard]] bool IsBroken() const noexcept;

  // Resolves a target string ("", "default", explicit node/sink.monitor) from the live cache.
  [[nodiscard]] ResolvedCaptureTarget ResolveTarget(std::string_view target) const;

  // Thread-safe snapshots of cached registry state.
  [[nodiscard]] std::vector<DeviceInfo> SnapshotDevices() const;
  [[nodiscard]] std::string DefaultSourceName() const;

  // Pimpl state, defined in pipewire_device.cpp. The forward declaration is public because the
  // control-plane event callbacks (free functions in that TU) must name it when casting the
  // listener's data pointer; the type itself stays opaque outside this TU.
  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

std::vector<DeviceInfo> EnumerateAudioSources();

} // namespace vinput::pw
