#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx-utils/log.h>
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

void VinputEngine::cancelInterruptedRecording() {
  cancelPendingStop();
  if (modifier_hold_event_ && modifier_hold_event_->isEnabled()) {
    modifier_hold_event_->setEnabled(false);
  }
  if (session_ && (session_->phase == Session::Phase::Recording ||
                   session_->phase == Session::Phase::PendingStart)) {
    auto* target_ic = session_->ic;
    callCancelOperation(false);
    finishFrontendSession(target_ic);
    clearVoicePresentation(target_ic);
  }
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

  const auto origKey = keyEvent.origKey().normalize();
  const bool isModifier = origKey.isModifier();

  // Helper to classify single-modifier actions
  auto checkModifierAction = [&](const fcitx::Key& k) -> ModifierAction {
    if (!k.isModifier()) {
      return ModifierAction::None;
    }
    if (k.checkKeyList(trigger_keys_)) {
      return ModifierAction::Dictation;
    }
    if (k.checkKeyList(command_keys_)) {
      return ModifierAction::Command;
    }
    if (k.checkKeyList(menu_keys_)) {
      return ModifierAction::Menu;
    }
    return ModifierAction::None;
  };

  const ModifierAction modAction = checkModifierAction(origKey);

  // 3. Key Press Phase
  if (!keyEvent.isRelease()) {
    // 3.1 Single-modifier shortcut press
    if (modAction != ModifierAction::None) {
      if (keyEvent.rawKey().states().test(fcitx::KeyState::Repeat)) {
        keyEvent.filter();
        return;
      }

      // If already recording in Tap mode, pressing trigger again toggles it off
      if (session_ && session_->phase == Session::Phase::Recording && session_->trigger_released) {
        finishStopRecording();
        keyEvent.filterAndAccept();
        return;
      }

      if (modifier_hold_event_ && modifier_hold_event_->isEnabled()) {
        modifier_hold_event_->setEnabled(false);
      }
      pending_modifier_.action = modAction;
      pending_modifier_.key = origKey;
      pending_modifier_.press_time = std::chrono::steady_clock::now();
      pending_modifier_.ic = ic;
      modifier_hold_active_ = false;

      // Start hold timer for dictation/command if in Hold or Both mode
      if (modAction != ModifierAction::Menu &&
          (trigger_mode_ == TriggerMode::Hold || trigger_mode_ == TriggerMode::Both)) {
        const auto fire_at_usec =
            fcitx::now(kDefaultClock) +
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(hold_activation_delay_)
                    .count());

        modifier_hold_event_ = instance_->eventLoop().addTimeEvent(
            kDefaultClock, fire_at_usec, 0,
            [this, target_ic = ic, action = modAction, trigger = origKey](auto*, uint64_t) {
              if (target_ic == nullptr) {
                pending_modifier_.reset();
                return false;
              }
              modifier_hold_active_ = true;
              startVoiceRecording(target_ic, trigger, action == ModifierAction::Command);
              if (session_) {
                session_->stop_on_release = true;
              }
              return false;
            });
        modifier_hold_event_->setOneShot();
      }

      // Pass modifier press through to client to preserve shortcut chords
      keyEvent.filter();
      return;
    }

    // 3.2 Any non-matching key pressed while a modifier is pending or active
    // This indicates a combination (e.g. Ctrl+C, Alt+Tab, Shift+A). Interrupt and pass through!
    if (pending_modifier_.action != ModifierAction::None || modifier_hold_active_ ||
        (session_ && session_->stop_on_release && !session_->trigger_released)) {
      if (modifier_hold_event_ && modifier_hold_event_->isEnabled()) {
        modifier_hold_event_->setEnabled(false);
      }
      if (modifier_hold_active_ ||
          (session_ && session_->stop_on_release && !session_->trigger_released)) {
        cancelInterruptedRecording();
      }
      pending_modifier_.reset();
      modifier_hold_active_ = false;
      // Let the interrupting key pass through untouched to the client application
      return;
    }

    // 3.3 Non-modifier trigger keys (e.g. F8, Pause, etc.)
    const int trigger_index = !isModifier ? keyEvent.key().keyListIndex(trigger_keys_) : -1;
    const bool is_trigger = trigger_index >= 0;
    const int command_index = !isModifier ? keyEvent.key().keyListIndex(command_keys_) : -1;
    const bool is_command = command_index >= 0;

    if (is_trigger || is_command) {
      auto now = std::chrono::steady_clock::now();
      const auto since_last = now - last_trigger_time_;
      last_trigger_time_ = now;
      if (since_last < kTriggerDebounce) {
        keyEvent.filterAndAccept();
        return;
      }

      dismissMenusForVoiceActivity();
      cancelPendingStop();

      if (session_ && session_->phase == Session::Phase::Recording && session_->trigger_released) {
        finishStopRecording();
        keyEvent.filterAndAccept();
        return;
      }
      if (session_) {
        ensureStatusSync();
        keyEvent.filterAndAccept();
        return;
      }

      auto trigger = is_trigger ? trigger_keys_[trigger_index] : command_keys_[command_index];

      if (trigger_mode_ == TriggerMode::Hold) {
        cancelPendingStart();
        const auto fire_at_usec =
            fcitx::now(kDefaultClock) +
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(hold_activation_delay_)
                    .count());
        pending_start_event_ = instance_->eventLoop().addTimeEvent(
            kDefaultClock, fire_at_usec, 0,
            [this,
             ic_ref = ic != nullptr ? ic->watch()
                                    : fcitx::TrackableObjectReference<fcitx::InputContext>(),
             trigger, is_command](auto*, uint64_t) {
              auto* target_ic = ic_ref.get();
              if (target_ic == nullptr) {
                pending_start_event_.reset();
                return false;
              }
              startVoiceRecording(target_ic, trigger, is_command);
              if (session_) {
                session_->stop_on_release = true;
              }
              pending_start_event_.reset();
              return false;
            });
        pending_start_event_->setOneShot();
      } else {
        startVoiceRecording(ic, trigger, is_command);
      }
      keyEvent.filterAndAccept();
      return;
    }

    // Check non-modifier command palette shortcut
    if (!isModifier && handleCommandPaletteHotkey(keyEvent)) {
      return;
    }
  }

  // 4. Key Release Phase
  if (keyEvent.isRelease()) {
    // 4.1 Single-modifier release (native isReleaseOfModifier disambiguation)
    if (pending_modifier_.action != ModifierAction::None &&
        origKey.isReleaseOfModifier(pending_modifier_.key)) {
      if (modifier_hold_event_ && modifier_hold_event_->isEnabled()) {
        modifier_hold_event_->setEnabled(false);
      }
      const auto action = pending_modifier_.action;
      const auto trigger_key = pending_modifier_.key;
      auto* target_ic = pending_modifier_.ic;
      const bool was_hold = modifier_hold_active_;
      pending_modifier_.reset();
      modifier_hold_active_ = false;

      if (target_ic == nullptr) {
        keyEvent.filter();
        return;
      }

      if (was_hold) {
        // Hold mode: stop recording and recognize on release
        if (session_ && session_->phase == Session::Phase::Recording) {
          session_->trigger_released = true;
          scheduleStopRecording();
        } else if (session_ && session_->phase == Session::Phase::PendingStart) {
          session_->trigger_released = true;
          session_->stop_on_release = true;
        }
      } else {
        // Tap mode: short press (< hold delay) toggles recording or opens menu
        if (action == ModifierAction::Menu) {
          toggleCommandPalette(target_ic);
        } else if (trigger_mode_ == TriggerMode::Tap || trigger_mode_ == TriggerMode::Both) {
          startVoiceRecording(target_ic, trigger_key, action == ModifierAction::Command);
          if (session_) {
            session_->trigger_released = true;
            session_->stop_on_release = false;
          }
        }
      }

      // Filter and accept the release so clients like Firefox do not toggle their menu bars
      keyEvent.filterAndAccept();
      return;
    }

    // 4.2 Ongoing recording release tracking for hold-to-talk
    if (session_ && session_->stop_on_release && !session_->trigger_released &&
        isReleaseOfActiveTrigger(keyEvent.key())) {
      session_->trigger_released = true;
      if (session_->phase == Session::Phase::Recording) {
        scheduleStopRecording();
      }
      keyEvent.filterAndAccept();
      return;
    }

    // 4.3 Non-modifier trigger release handling
    const int trigger_index = !isModifier ? keyEvent.key().keyListIndex(trigger_keys_) : -1;
    const bool is_trigger = trigger_index >= 0;
    const int command_index = !isModifier ? keyEvent.key().keyListIndex(command_keys_) : -1;
    const bool is_command = command_index >= 0;

    if (is_trigger || is_command) {
      if (trigger_mode_ == TriggerMode::Hold && pending_start_event_ &&
          pending_start_event_->isEnabled()) {
        cancelPendingStart();
        keyEvent.filterAndAccept();
        return;
      }
      if (session_) {
        session_->trigger_released = true;
      }
      keyEvent.filterAndAccept();
      return;
    }
  }
}

bool VinputEngine::handleCommandPaletteHotkey(fcitx::KeyEvent& keyEvent) {
  const auto event_key = keyEvent.key();
  if (event_key.isModifier() || !event_key.checkKeyList(menu_keys_)) {
    return false;
  }

  if (!keyEvent.isRelease()) {
    if (session_) {
      return false;
    }
    toggleCommandPalette(keyEvent.inputContext());
  }
  keyEvent.filterAndAccept();
  return true;
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

bool VinputEngine::isReleaseOfActiveTrigger(const fcitx::Key& key) const {
  if (!session_) {
    return false;
  }

  const auto release_key = key.normalize();
  const auto trigger_key = session_->trigger.normalize();

  if (trigger_key.isModifier() && release_key.isReleaseOfModifier(trigger_key)) {
    return true;
  }

  if (release_key.sym() == trigger_key.sym()) {
    if (trigger_key.states().toInteger() == 0) {
      return true;
    }
    return release_key.states().testAny(trigger_key.states()) &&
           (release_key.states() & trigger_key.states()) == trigger_key.states();
  }

  const auto released_modifier_state = fcitx::Key::keySymToStates(release_key.sym());
  return released_modifier_state.toInteger() != 0 &&
         trigger_key.states().testAny(released_modifier_state);
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
  enterBusyState(session_->ic, session_->command_mode, _("... Recognizing ..."));
  callStopRecording(scene.id);
}
