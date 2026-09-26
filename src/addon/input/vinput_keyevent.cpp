#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <iterator>
#include <string>
#include <utility>

#include "common/config/core_config.h"
#include "common/config/vinput_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/debug_log.h"

#include "clipboard_public.h"
#include "core/vinput.h"

namespace {

constexpr auto kDefaultClock = 1; // POSIX CLOCK_MONOTONIC
constexpr auto kReleaseDebounce = std::chrono::milliseconds(500);
constexpr auto kTriggerDebounce = std::chrono::milliseconds(80);
constexpr auto kTapMaxHoldDuration = std::chrono::milliseconds(200);

std::string NoSelectionPreeditText() {
  return _("Please select text first.");
}

std::string CommandDisabledPreeditText() {
  return _("Command mode is disabled (candidate count is 0).");
}
std::string CommandNoProviderPreeditText() {
  return _("No LLM provider configured for command mode.");
}
std::string DaemonUnavailablePreeditText() {
  return _("Voice input daemon is temporarily unavailable.");
}
std::string DaemonNotRespondingPreeditText() {
  return _("Voice input daemon is not responding.");
}

} // namespace

int VinputEngine::matchKeyListIndex(const fcitx::Key& event_key, const fcitx::KeyList& list,
                                    bool is_release) {
  const int idx = event_key.keyListIndex(list);
  if (idx >= 0) {
    return idx;
  }
  if (is_release && event_key.isModifier()) {
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (event_key.isReleaseOfModifier(list[i])) {
        return static_cast<int>(i);
      }
    }
  }
  return -1;
}

void VinputEngine::startVoiceRecording(fcitx::InputContext* ic, const fcitx::Key& trigger,
                                       bool is_command) {
  if (ic == nullptr) {
    return;
  }
  dismissMenusForVoiceActivity();
  cancelPendingStop();

  const std::string daemon_status = last_known_daemon_status_;
  if (!is_command && !session_ && daemon_status == vinput::dbus::kStatusRecording) {
    enterRecordingState(ic, trigger, false);
    finishStopRecording();
    return;
  }
  if (!daemon_status.empty() && daemon_status != vinput::dbus::kStatusIdle) {
    applyDaemonStatusLocally(daemon_status, ic, is_command);
    return;
  }

  if (is_command) {
    {
      auto core_config = LoadCoreConfig();
      const auto* cmd_scene = FindCommandScene(core_config);
      if (cmd_scene == nullptr || cmd_scene->llm_max_candidates <= 0) {
        finishFrontendSession(ic);
        updateVoicePresentation(ic, CommandDisabledPreeditText());
        return;
      }
      if (cmd_scene->provider_id.empty() ||
          ResolveLlmProvider(core_config, cmd_scene->provider_id) == nullptr) {
        finishFrontendSession(ic);
        updateVoicePresentation(ic, CommandNoProviderPreeditText());
        return;
      }
    }
    std::string selected_text;
    auto& surrounding = ic->surroundingText();
    if (surrounding.isValid() && surrounding.cursor() != surrounding.anchor()) {
      const auto& text = surrounding.text();
      auto char_from = std::min(surrounding.cursor(), surrounding.anchor());
      auto char_to = std::max(surrounding.cursor(), surrounding.anchor());
      if (fcitx::utf8::validate(text)) {
        auto byte_from = fcitx::utf8::ncharByteLength(text.begin(), char_from);
        auto byte_len =
            fcitx::utf8::ncharByteLength(std::next(text.begin(), byte_from), char_to - char_from);
        selected_text = text.substr(byte_from, byte_len);
      }
    }
    if (selected_text.empty()) {
      if (auto* clipboard = instance_->addonManager().addon("clipboard")) {
        auto primary = clipboard->call<fcitx::IClipboard::primary>(ic);
        if (fcitx::utf8::validate(primary)) {
          selected_text = std::move(primary);
        }
      }
    }
    if (selected_text.empty()) {
      if (status_ic_ == ic) {
        finishFrontendSession(ic);
      } else {
        clearVoicePresentation(ic);
      }
      vinput::debug::Log("command trigger ignored because no selection text is available\n");
      updateVoicePresentation(ic, NoSelectionPreeditText());
      return;
    }
    enterPendingStartState(ic, trigger, true);
    FCITX_LOG(Debug) << "vinput: command key activated, selected_text length="
                     << selected_text.size();
    if (!callStartCommandRecording(selected_text)) {
      finishFrontendSession(ic);
      if (bus_ == nullptr) {
        vinput::debug::Log("command trigger fallback: daemon bus unavailable\n");
        updateVoicePresentation(ic, DaemonUnavailablePreeditText());
      } else if (!daemonSyncAllowed()) {
        vinput::debug::Log(
            "command trigger fallback: daemon sync throttled after timeout/failure\n");
        updateVoicePresentation(ic, DaemonNotRespondingPreeditText());
      }
    }
  } else {
    enterPendingStartState(ic, trigger, false);
    FCITX_LOG(Debug) << "vinput: trigger key activated";
    if (!callStartRecording()) {
      finishFrontendSession(ic);
      if (bus_ == nullptr) {
        vinput::debug::Log("record trigger fallback: daemon bus unavailable\n");
        updateVoicePresentation(ic, DaemonUnavailablePreeditText());
      } else if (!daemonSyncAllowed()) {
        vinput::debug::Log(
            "record trigger fallback: daemon sync throttled after timeout/failure\n");
        updateVoicePresentation(ic, DaemonNotRespondingPreeditText());
      }
    }
  }
}

void VinputEngine::handleKeyEvent(fcitx::Event& event) {
  auto& keyEvent = static_cast<fcitx::KeyEvent&>(event);
  auto* ic = keyEvent.inputContext();
  rememberInputContext(ic);

  // 1. Postprocessing cancellation handling (Escape = discard, Enter = commit raw)
  if (pending_postprocessing_release_ && keyEvent.isRelease() &&
      keyEvent.key().normalize().sym() == pending_postprocessing_release_->normalize().sym()) {
    pending_postprocessing_release_.reset();
    keyEvent.filterAndAccept();
    return;
  }

  if (session_ && session_->phase == Session::Phase::Postprocessing &&
      last_known_daemon_status_ == vinput::dbus::kStatusPostprocessing) {
    const bool discard = keyEvent.key().check(FcitxKey_Escape);
    const bool commit_raw =
        session_->raw_prev && !session_->command_mode &&
        (keyEvent.key().check(FcitxKey_Return) || keyEvent.key().check(FcitxKey_KP_Enter));
    if (discard || commit_raw) {
      if (!keyEvent.isRelease()) {
        pending_postprocessing_release_ = keyEvent.key();
        callCancelOperation(commit_raw);
      }
      keyEvent.filterAndAccept();
      return;
    }
  }

  // 2. Active menus consume keyboard navigation first
  if (result_menu_visible_ && handleResultMenuKeyEvent(keyEvent)) {
    return;
  }
  if (palette_menu_visible_ && handlePaletteMenuKeyEvent(keyEvent)) {
    return;
  }

  const auto event_key = keyEvent.origKey().normalize();

  // 3. Classify hotkeys using physical sym match (immune to XKB modifier release state bits)
  const int trigger_index = matchKeyListIndex(event_key, trigger_keys_, keyEvent.isRelease());
  const bool is_trigger = trigger_index >= 0;
  const int command_index = matchKeyListIndex(event_key, command_keys_, keyEvent.isRelease());
  const bool is_command = !is_trigger && command_index >= 0;
  const int menu_index = matchKeyListIndex(event_key, menu_keys_, keyEvent.isRelease());
  const bool is_menu = !is_trigger && !is_command && menu_index >= 0;

  HotkeyRole role = HotkeyRole::None;
  if (is_trigger) {
    role = HotkeyRole::Trigger;
  } else if (is_command) {
    role = HotkeyRole::Command;
  } else if (is_menu) {
    role = HotkeyRole::Menu;
  }

  // 4. Non-hotkey (regular keys)
  if (role == HotkeyRole::None) {
    if (!keyEvent.isRelease()) {
      // Intervening key breaks any active hotkey into a chord
      if (held_role_ != HotkeyRole::None) {
        chord_interrupted_ = true;
        cancelPendingStart();

        // If an optimistic or hold recording is ongoing, discard it immediately
        if (session_ && session_->stop_on_release && !session_->trigger_released) {
          auto* target_ic = session_->ic;
          callCancelOperation(false);
          finishFrontendSession(target_ic);
          clearVoicePresentation(target_ic);
        }
      }
    }
    // Pass the non-hotkey untouched to client
    return;
  }

  fcitx::Key matched_key;
  if (is_trigger) {
    matched_key = trigger_keys_[trigger_index];
  } else if (is_command) {
    matched_key = command_keys_[command_index];
  } else {
    matched_key = menu_keys_[menu_index];
  }

  // 5. Press Phase
  if (!keyEvent.isRelease()) {
    auto now = std::chrono::steady_clock::now();

    if (role == HotkeyRole::Trigger || role == HotkeyRole::Command) {
      const auto since_last = now - last_trigger_time_;
      last_trigger_time_ = now;
      if (since_last < kTriggerDebounce) {
        keyEvent.filterAndAccept();
        return;
      }

      dismissMenusForVoiceActivity();
      cancelPendingStop();

      // If recording is active and initiated by this trigger:
      if (session_ && (session_->phase == Session::Phase::Recording ||
                       session_->phase == Session::Phase::PendingStart)) {
        if (session_->trigger == matched_key) {
          if (session_->trigger_released) {
            // Tap toggle: second press stops recording
            if (session_->phase == Session::Phase::PendingStart) {
              auto* target_ic = session_->ic;
              callCancelOperation(false);
              finishFrontendSession(target_ic);
              clearVoicePresentation(target_ic);
            } else {
              finishStopRecording();
            }
          }
          // If trigger_released is false, this is an auto-repeat while holding: swallow it!
        } else {
          // Interrupted by a different trigger key: cancel active recording
          auto* target_ic = session_->ic;
          callCancelOperation(false);
          finishFrontendSession(target_ic);
          clearVoicePresentation(target_ic);
        }
        keyEvent.filterAndAccept();
        return;
      }
    }

    // New press
    held_key_sym_ = event_key.sym();
    held_role_ = role;
    chord_interrupted_ = false;
    held_press_time_ = now;

    if (role == HotkeyRole::Trigger || role == HotkeyRole::Command) {
      const auto trigger_mode = *config_.triggerMode;
      if (trigger_mode == TriggerMode::Hold || trigger_mode == TriggerMode::Both) {
        // 0-latency immediate recording start (never swallow first syllable!)
        startVoiceRecording(ic, matched_key, is_command);
        if (session_) {
          session_->press_time = now;
          session_->stop_on_release = true;
          session_->trigger_released = false;
        }
      }
    }

    keyEvent.filterAndAccept();
    return;
  }

  // 6. Release Phase
  if (keyEvent.isRelease()) {
    const bool matches_active =
        (held_role_ == role) && held_key_sym_.has_value() && *held_key_sym_ == event_key.sym();

    if (matches_active) {
      const auto active_role = held_role_;
      held_role_ = HotkeyRole::None;
      held_key_sym_.reset();

      // If interrupted by another key (e.g. Alt+Tab, Alt+T, Shift+A), release does NOT trigger
      // action
      if (chord_interrupted_) {
        chord_interrupted_ = false;
        cancelPendingStart();
        keyEvent.filterAndAccept();
        return;
      }

      auto now = std::chrono::steady_clock::now();
      const auto duration = now - held_press_time_;

      if (active_role == HotkeyRole::Menu) {
        if (!session_) {
          toggleCommandPalette(ic);
        }
      } else {
        const auto trigger_mode = *config_.triggerMode;
        if (trigger_mode == TriggerMode::Hold) {
          // Pure Hold: stop on release
          if (session_ && session_->phase == Session::Phase::Recording) {
            scheduleStopRecording();
          }
        } else if (trigger_mode == TriggerMode::Tap) {
          // Pure Tap: toggle on release
          if (!session_) {
            startVoiceRecording(ic, matched_key, is_command);
            if (session_) {
              session_->press_time = now;
              session_->trigger_released = true;
              session_->stop_on_release = false;
            }
          } else if (session_->phase == Session::Phase::Recording) {
            scheduleStopRecording();
          }
        } else { // TriggerMode::Both
          if (duration < kTapMaxHoldDuration) {
            // Short tap (< 200ms): keep recording going continuously!
            if (session_) {
              session_->trigger_released = true;
              session_->stop_on_release = false;
            }
          } else {
            // Long hold (>= 200ms): user finished speaking, stop on release!
            if (session_ && session_->phase == Session::Phase::Recording) {
              scheduleStopRecording();
            }
          }
        }
      }
    }

    // Release during active recording
    if (session_ && session_->trigger == matched_key) {
      session_->trigger_released = true;
      if (session_->stop_on_release) {
        if (session_->phase == Session::Phase::Recording) {
          scheduleStopRecording();
        }
      }
    }

    keyEvent.filterAndAccept();
    return;
  }
}

void VinputEngine::toggleCommandPalette(fcitx::InputContext* ic) {
  if (palette_menu_visible_) {
    hidePaletteMenu();
    return;
  }
  if (session_) {
    return;
  }
  if (pending_start_event_ && pending_start_event_->isEnabled()) {
    return;
  }
  showPaletteMenu(ic);
}

bool VinputEngine::isPendingStartTrigger(const fcitx::Key& trigger) const {
  return pending_start_event_ && pending_start_event_->isEnabled() &&
         pending_start_trigger_ == trigger;
}

void VinputEngine::cancelPendingStop() {
  if (pending_stop_event_ && pending_stop_event_->isEnabled()) {
    pending_stop_event_->setEnabled(false);
  }
}

void VinputEngine::cancelPendingStart() {
  if (pending_start_event_ && pending_start_event_->isEnabled()) {
    pending_start_event_->setEnabled(false);
  }
  pending_start_trigger_ = fcitx::Key();
  pending_start_ic_.unwatch();
  held_role_ = HotkeyRole::None;
  held_key_sym_.reset();
}

void VinputEngine::scheduleStopRecording() {
  const auto fire_at_usec =
      fcitx::now(kDefaultClock) +
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(kReleaseDebounce).count());

  if (!pending_stop_event_) {
    pending_stop_event_ = instance_->eventLoop().addTimeEvent(kDefaultClock, fire_at_usec, 0,
                                                              [this](auto*, uint64_t) {
                                                                finishStopRecording();
                                                                return false;
                                                              });
    pending_stop_event_->setOneShot();
    return;
  }

  pending_stop_event_->setTime(fire_at_usec);
  pending_stop_event_->setEnabled(true);
}

void VinputEngine::finishStopRecording() {
  if (!session_.has_value() || session_->phase != Session::Phase::Recording) {
    return;
  }

  reloadSceneConfig();
  if (!session_.has_value()) {
    return;
  }
  const auto& scene = vinput::scene::Resolve(scene_config_, active_scene_id_);
  active_scene_id_ = scene.id;
  session_->raw_prev = scene.raw_prev;
  session_->trigger = fcitx::Key();
  enterBusyState(session_->ic, session_->command_mode, _("... Recognizing ..."), false,
                 scene.raw_prev);
  callStopRecording(scene.id);
}
