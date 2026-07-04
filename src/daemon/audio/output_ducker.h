#pragma once

#include <mutex>

namespace vinput::daemon::audio {

// Temporarily lowers the default audio sink volume while recording is active,
// then restores it afterwards. Volume is driven through WirePlumber (wpctl) so
// that it matches the system volume the user sees and hears, stays consistent
// with the session manager, and is fully restorable. All operations are
// best-effort: if wpctl is unavailable (e.g. a Flatpak sandbox) or fails, the
// call is a no-op so that ducking can never block recording.
class OutputDucker {
public:
  OutputDucker() = default;
  ~OutputDucker() = default;

  OutputDucker(const OutputDucker &) = delete;
  OutputDucker &operator=(const OutputDucker &) = delete;

  // Lower the default sink to (current_volume * scale). scale is clamped to
  // [0.0, 1.0]. Idempotent: a second call while already ducked is a no-op.
  void Duck(double scale);

  // Restore the volume saved by the most recent Duck(). No-op if not ducked.
  void Restore();

private:
  std::mutex mutex_;
  bool ducked_ = false;
  double saved_volume_ = 0.0;
};

}  // namespace vinput::daemon::audio
