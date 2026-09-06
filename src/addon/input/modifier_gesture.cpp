#include "modifier_gesture.h"

#include <algorithm>
#include <utility>

namespace {
constexpr auto kChordReleaseTimeout = std::chrono::milliseconds(200);
}

fcitx::KeyStates ModifierGesture::modifiers(const fcitx::Key& key) {
  // Ignore locks, repeat/handled flags, and pointer state. Pointer gestures are
  // outside this recognizer's scope.
  return key.normalize().states() &
         (fcitx::KeyStates(fcitx::KeyState::SimpleMask) | fcitx::KeyState::Mod5);
}

fcitx::KeyStates ModifierGesture::requiredModifiers(const fcitx::Key& key) {
  return modifiers(key) | fcitx::Key::keySymToStates(key.normalize().sym());
}

void ModifierGesture::setBindings(std::vector<Binding> bindings) {
  cancel();
  bindings_ = std::move(bindings);
}

void ModifierGesture::clearGesture() {
  binding_.reset();
  first_release_.reset();
  fired_ = false;
}

void ModifierGesture::cancel() {
  clearGesture();
  pressed_.clear();
  cancelled_ = false;
}

bool ModifierGesture::matches(const Binding& binding, fcitx::KeyStates down) const {
  const auto key = binding.key.normalize();
  if (down != requiredModifiers(key)) {
    return false;
  }
  return std::any_of(pressed_.begin(), pressed_.end(),
                     [&](const auto& pressed) { return pressed.sym == key.sym(); });
}

ModifierGesture::Result ModifierGesture::keyEvent(const fcitx::Key& event_key, bool release,
                                                  Clock::time_point now, bool toggle_recording) {
  const auto key = event_key.normalize();
  const auto same_key = [&](const auto& pressed) {
    return key.code() && pressed.code ? key.code() == pressed.code : event_key.sym() == pressed.sym;
  };
  auto found = std::find_if(pressed_.begin(), pressed_.end(), same_key);
  auto consumed = std::find_if(consumed_.begin(), consumed_.end(), same_key);
  const bool repeated =
      !release && (found != pressed_.end() || event_key.states().test(fcitx::KeyState::Repeat));
  // Use the press identity on release: layouts can change the release keysym.
  const auto own =
      found == pressed_.end() ? fcitx::Key::keySymToStates(key.sym()) : found->modifier;
  const bool was_modifier = own.toInteger() != 0;
  Result result;
  // Once a press is consumed, its repeats and release belong to us even if
  // recognition was cancelled or reset in the meantime.
  result.filter = consumed != consumed_.end() && (release || repeated);
  if (consumed != consumed_.end() && (release || !repeated)) {
    consumed_.erase(consumed);
  }
  if (release && found != pressed_.end()) {
    pressed_.erase(found);
  } else if (!release && !repeated) {
    pressed_.push_back({event_key.sym(), key.code(), own});
  }
  auto down = modifiers(key);
  if (release) {
    down &= ~own;
  } else {
    down |= own;
  }
  for (const auto& pressed : pressed_) {
    down |= pressed.modifier;
  }
  if (repeated) {
    return result;
  }

  if (release) {
    if (binding_ && was_modifier) {
      if (!first_release_ && (requiredModifiers(binding_->key) & own).toInteger() != 0) {
        first_release_ = now;
        if (fired_) {
          result.event = Event::HoldRelease;
          result.binding = binding_;
        }
      }
      if (down.toInteger() == 0) {
        if (!cancelled_ && !fired_ && first_release_ &&
            now - *first_release_ <= kChordReleaseTimeout) {
          const auto held = *first_release_ - pressed_at_;
          if ((binding_->mode == Mode::Tap ||
               (binding_->mode == Mode::Both && held < binding_->hold_delay)) &&
              (!binding_->tap_timeout || held <= *binding_->tap_timeout)) {
            result.event = Event::Tap;
            result.binding = binding_;
          }
        }
        clearGesture();
      }
    }
    if (pressed_.empty() && down.toInteger() == 0) {
      clearGesture();
      cancelled_ = false;
    }
    return result;
  }

  // An extra press aborts an active hold. Its first required release has
  // already ended the gesture, so later typing must not discard that recording.
  if (fired_) {
    if (!first_release_) {
      result = {Event::HoldCancel, binding_, false};
      clearGesture();
      cancelled_ = true;
    }
    return result;
  }
  if (first_release_ || !was_modifier ||
      std::any_of(pressed_.begin(), pressed_.end(),
                  [](const auto& pressed) { return pressed.modifier.toInteger() == 0; })) {
    cancelled_ = true;
  }
  if (cancelled_) {
    binding_.reset();
    return result;
  }

  // A pending Ctrl gesture may grow into a configured Ctrl+Alt+Shift chord.
  // Select only an exact completed chord, never a subset of extra modifiers.
  binding_.reset();
  for (auto binding : bindings_) {
    if (matches(binding, down)) {
      if (toggle_recording && binding.action != Action::Palette) {
        binding.mode = Mode::Tap;
      }
      binding_ = binding;
      pressed_at_ = now;
      // A standalone binding reserves its key. Chords only observe modifier
      // events so configuring Ctrl+Alt does not reserve Ctrl or Alt by itself.
      result.filter =
          requiredModifiers(binding.key) == own && event_key.sym() == binding.key.normalize().sym();
      break;
    }
  }
  if (!binding_) {
    const bool prefix = std::any_of(bindings_.begin(), bindings_.end(), [&](const auto& binding) {
      return (down & requiredModifiers(binding.key)) == down;
    });
    cancelled_ = !prefix;
  }
  if (result.filter) {
    consumed_.push_back(pressed_.back());
  }
  return result;
}

std::optional<ModifierGesture::Clock::time_point> ModifierGesture::deadline() const {
  if (!binding_ || cancelled_ || fired_ || first_release_ || binding_->mode == Mode::Tap) {
    return std::nullopt;
  }
  return pressed_at_ + binding_->hold_delay;
}

ModifierGesture::Result ModifierGesture::timeout(Clock::time_point now) {
  const auto at = deadline();
  if (!at || now < *at) {
    return {};
  }
  fired_ = true;
  return {Event::HoldStart, binding_, false};
}
