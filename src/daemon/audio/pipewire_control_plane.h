#pragma once

#include <memory>
#include <string_view>

#include "common/audio/pipewire_device.h"

struct pw_core;
struct pw_thread_loop;

namespace vinput::daemon::audio {

// Persistent PipeWire control-plane watcher internal to daemon audio capture.
// Keeps an incremental cache of audio nodes (including object.serial) and
// the "default.audio.source" metadata key without creating streams.
class PipeWireControlPlane {
public:
  PipeWireControlPlane();
  ~PipeWireControlPlane();

  PipeWireControlPlane(const PipeWireControlPlane&) = delete;
  PipeWireControlPlane& operator=(const PipeWireControlPlane&) = delete;
  PipeWireControlPlane(PipeWireControlPlane&&) = delete;
  PipeWireControlPlane& operator=(PipeWireControlPlane&&) = delete;

  // Attaches a started pw_thread_loop and connects core/registry/metadata watchers.
  // Returns false without failing permanently if PipeWire is temporarily unavailable.
  bool Connect(pw_thread_loop* thread_loop, int sync_timeout_ms = 250);

  // Ensures the persistent connection is healthy, reconnecting after a PipeWire restart or
  // deferred startup connection. Caller must NOT hold the thread_loop lock.
  bool EnsureConnected(pw_thread_loop* thread_loop, int sync_timeout_ms = 250);

  // Tears down all control-plane proxies under the attached thread_loop lock before stopping
  // the loop. Automatically invoked by ~PipeWireControlPlane() if still attached.
  void Disconnect() noexcept;

  // Returns the persistent core handle for pw_stream_new (valid while EnsureConnected holds
  // under thread_loop lock), or nullptr if disconnected.
  [[nodiscard]] pw_core* Core() const noexcept;

  // Resolves a target string ("", "default", explicit node/sink.monitor) from the live cache.
  [[nodiscard]] vinput::pw::ResolvedCaptureTarget ResolveTarget(std::string_view target) const;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace vinput::daemon::audio
