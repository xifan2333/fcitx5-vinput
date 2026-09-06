#pragma once

#include <chrono>
#include <fcitx-utils/key.h>
#include <optional>
#include <vector>

// Recognizes complete modifier chords independently of recording/UI side effects.
// Like KDE's modifier shortcuts, taps finish after all modifiers are released.
// Fcitx bindings retain their explicit left/right keysym (e.g. Control+Shift_L).
class ModifierGesture {
public:
  using Clock = std::chrono::steady_clock;
  using Duration = std::chrono::milliseconds;
  enum class Action { Dictation, Command, Palette };
  enum class Mode { Tap, Hold, Both };
  enum class Event { None, Tap, HoldStart, HoldRelease, HoldCancel };
  struct Binding {
    fcitx::Key key;
    Action action;
    Mode mode;
    Duration hold_delay;
    std::optional<Duration> tap_timeout = std::nullopt;
  };
  struct Result {
    Event event = Event::None;
    std::optional<Binding> binding;
    bool filter = false;
  };

  void setBindings(std::vector<Binding> bindings);
  // Cancel recognition, retaining pressed-key ownership until physical release.
  void cancel();
  Result keyEvent(const fcitx::Key& key, bool release, Clock::time_point now,
                  bool toggle_recording = false);
  Result timeout(Clock::time_point now);
  std::optional<Clock::time_point> deadline() const;

private:
  struct PressedKey {
    fcitx::KeySym sym;
    int code;
    fcitx::KeyStates modifier;
  };
  static fcitx::KeyStates modifiers(const fcitx::Key& key);
  static fcitx::KeyStates requiredModifiers(const fcitx::Key& key);
  bool matches(const Binding& binding, fcitx::KeyStates down) const;
  void clearGesture();

  std::vector<Binding> bindings_;
  std::vector<PressedKey> pressed_;
  std::vector<PressedKey> consumed_;
  std::optional<Binding> binding_;
  Clock::time_point pressed_at_{};
  std::optional<Clock::time_point> first_release_;
  bool cancelled_ = false;
  bool fired_ = false;
};
