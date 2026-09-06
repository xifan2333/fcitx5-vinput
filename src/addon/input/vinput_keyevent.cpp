#include <chrono>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <string>

#include "common/config/core_config.h"
#include "common/config/vinput_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/debug_log.h"

#include "clipboard_public.h"
#include "core/vinput.h"

namespace {

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

void VinputEngine::handleKeyEvent(fcitx::Event& event) {
  auto& keyEvent = static_cast<fcitx::KeyEvent&>(event);
  rememberInputContext(keyEvent.inputContext());
  if (modifier_ic_ && modifier_ic_ != keyEvent.inputContext()) {
    resetPendingGestures();
  }
  modifier_ic_ = keyEvent.inputContext();
  const auto gesture = modifier_gesture_.keyEvent(
      keyEvent.rawKey(), keyEvent.isRelease(), std::chrono::steady_clock::now(),
      session_ && session_->trigger_released &&
          (session_->phase == Session::Phase::Recording ||
           session_->phase == Session::Phase::PendingStart));
  // The session retains ownership if a focus/config reset cleared the recognizer.
  const bool interrupted_modifier_hold =
      session_ && session_->trigger.isModifier() && session_->stop_on_release &&
      !session_->trigger_released && !keyEvent.isRelease() &&
      !keyEvent.rawKey().states().test(fcitx::KeyState::Repeat) &&
      !isReleaseOfActiveTrigger(keyEvent.rawKey());
  if (gesture.event == ModifierGesture::Event::HoldCancel || interrupted_modifier_hold) {
    cancelModifierRecording();
    updateModifierTimer();
    return; // Let the interrupting shortcut reach the application.
  }
  // Context resets may clear recognition state after a hold has started. Still
  // stop on release, including while StartRecording is in flight.
  const bool held_modifier_release =
      !recording_cancel_requested_ && session_ && session_->trigger.isModifier() &&
      session_->stop_on_release && keyEvent.isRelease() && isReleaseOfActiveTrigger(keyEvent.key());
  if (held_modifier_release && !session_->trigger_released) {
    session_->trigger_released = true;
    if (session_->phase == Session::Phase::Recording) {
      scheduleStopRecording();
    }
  }
  dispatchModifierGesture(gesture, keyEvent.inputContext());
  updateModifierTimer();
  if (gesture.filter) {
    keyEvent.filterAndAccept();
    return;
  }

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
  const int trigger_index = keyEvent.key().keyListIndex(trigger_keys_);
  const bool is_trigger = !keyEvent.key().isModifier() && trigger_index >= 0;
  const int command_index = keyEvent.key().keyListIndex(command_keys_);
  const bool is_command = !keyEvent.key().isModifier() && command_index >= 0;
  const bool voice_trigger_press = (is_trigger || is_command) && !keyEvent.isRelease();
  const bool voice_start_pending = pending_start_event_ && pending_start_event_->isEnabled();

  if (trigger_mode_ == TriggerMode::Hold && (is_trigger || is_command) && keyEvent.isRelease() &&
      voice_start_pending) {
    cancelPendingStart();
    keyEvent.filterAndAccept();
    return;
  }

  if (!voice_trigger_press) {
    if (!is_trigger && !is_command && handleCommandPaletteHotkey(keyEvent)) {
      return;
    }

    if (result_menu_visible_ && handleResultMenuKeyEvent(keyEvent)) {
      return;
    }

    if (palette_menu_visible_ && handlePaletteMenuKeyEvent(keyEvent)) {
      return;
    }
  }

  FCITX_LOG(Debug) << "vinput handleKeyEvent: " << keyEvent.key()
                   << " is_release=" << keyEvent.isRelease() << " is_trigger=" << is_trigger
                   << " is_command=" << is_command;

  if ((is_trigger || is_command) && !keyEvent.isRelease()) {
    // Auto-repeat is part of the same press: keep its original hold deadline
    // and never toggle recording again while the trigger remains held.
    if (keyEvent.rawKey().states().test(fcitx::KeyState::Repeat)) {
      keyEvent.filterAndAccept();
      return;
    }
    auto now = std::chrono::steady_clock::now();
    const auto since_last = now - last_trigger_time_;
    last_trigger_time_ = now;
    if (since_last < kTriggerDebounce) {
      keyEvent.filterAndAccept();
      return;
    }

    auto* ic = keyEvent.inputContext();
    const auto trigger = is_trigger ? trigger_keys_[trigger_index] : command_keys_[command_index];
    if (trigger_mode_ == TriggerMode::Hold && !session_ &&
        (last_known_daemon_status_.empty() ||
         last_known_daemon_status_ == vinput::dbus::kStatusIdle)) {
      dismissMenusForVoiceActivity();
      cancelPendingStop();
      cancelPendingStart();
      pending_start_ic_ = ic;
      const auto fire_at =
          fcitx::now(CLOCK_MONOTONIC) +
          std::chrono::duration_cast<std::chrono::microseconds>(hold_activation_delay_).count();
      pending_start_event_ = instance_->eventLoop().addTimeEvent(
          CLOCK_MONOTONIC, fire_at, 0,
          [this, ic, trigger, is_command](fcitx::EventSourceTime*, uint64_t) {
            activateVoiceTrigger(ic, trigger, is_command);
            if (session_) {
              session_->stop_on_release = true;
            }
            pending_start_ic_ = nullptr;
            return false;
          });
      pending_start_event_->setOneShot();
    } else {
      activateVoiceTrigger(ic, trigger, is_command);
    }
    keyEvent.filterAndAccept();
    return;
  }

  // Hold mode: cancel deferred start on early release, otherwise push-to-talk
  if (trigger_mode_ == TriggerMode::Hold && (is_trigger || is_command) && keyEvent.isRelease()) {
    if (pending_start_event_ && pending_start_event_->isEnabled()) {
      cancelPendingStart();
      keyEvent.filterAndAccept();
      return;
    }
    if (session_ && isReleaseOfActiveTrigger(keyEvent.key())) {
      const bool recording = session_->phase == Session::Phase::Recording;
      session_->trigger_released = true;
      session_->stop_on_release = true;
      if (recording) {
        scheduleStopRecording();
      }
    }
    keyEvent.filterAndAccept();
    return;
  }

  // Tap mode: release events are irrelevant (toggle on press only);
  // just mark trigger_released so the next press can toggle off.
  if (trigger_mode_ == TriggerMode::Tap && (is_trigger || is_command) && keyEvent.isRelease()) {
    if (session_) {
      session_->trigger_released = true;
    }
    keyEvent.filterAndAccept();
    return;
  }

  // Both mode: push-to-talk stop on release if held long enough
  if (session_ && session_->phase == Session::Phase::Recording && keyEvent.isRelease() &&
      !session_->trigger.isModifier() && isReleaseOfActiveTrigger(keyEvent.key())) {
    session_->trigger_released = true;
    auto held = std::chrono::steady_clock::now() - session_->press_time;
    if (held >= hold_activation_delay_) {
      scheduleStopRecording();
    }
    keyEvent.filterAndAccept();
    return;
  }

  // Both mode: mark trigger released for toggle
  if ((is_trigger || is_command) && keyEvent.isRelease()) {
    if (session_) {
      session_->trigger_released = true;
    }
    keyEvent.filterAndAccept();
    return;
  }
}

void VinputEngine::resetPendingGestures() {
  modifier_gesture_.cancel();
  modifier_ic_ = nullptr;
  if (modifier_timer_) {
    modifier_timer_->setEnabled(false);
  }
  cancelPendingStart();
}

void VinputEngine::updateModifierTimer() {
  if (modifier_timer_) {
    modifier_timer_->setEnabled(false);
  }
  const auto deadline = modifier_gesture_.deadline();
  if (!deadline) {
    return;
  }
  const auto remaining = std::max(std::chrono::steady_clock::duration::zero(),
                                  *deadline - std::chrono::steady_clock::now());
  const auto fire_at =
      fcitx::now(CLOCK_MONOTONIC) + std::chrono::ceil<std::chrono::microseconds>(remaining).count();
  modifier_timer_ = instance_->eventLoop().addTimeEvent(
      CLOCK_MONOTONIC, fire_at, 0, [this](fcitx::EventSourceTime*, uint64_t) {
        dispatchModifierGesture(modifier_gesture_.timeout(std::chrono::steady_clock::now()),
                                modifier_ic_);
        return false;
      });
  modifier_timer_->setOneShot();
}

void VinputEngine::dispatchModifierGesture(const ModifierGesture::Result& result,
                                           fcitx::InputContext* ic) {
  if (result.event == ModifierGesture::Event::None || !result.binding || !ic) {
    return;
  }
  const auto& binding = *result.binding;
  if (binding.action == ModifierGesture::Action::Palette) {
    if (result.event == ModifierGesture::Event::Tap) {
      toggleCommandPalette(ic);
    }
    return;
  }
  if (result.event == ModifierGesture::Event::HoldRelease) {
    if (session_ && session_->trigger == binding.key && session_->ic == ic &&
        !session_->trigger_released) {
      session_->trigger_released = true;
      if (session_->stop_on_release && session_->phase == Session::Phase::Recording) {
        scheduleStopRecording();
      }
    }
    return;
  }
  activateVoiceTrigger(ic, binding.key, binding.action == ModifierGesture::Action::Command);
  if (session_ && session_->trigger == binding.key && session_->ic == ic) {
    if (result.event == ModifierGesture::Event::Tap) {
      session_->trigger_released = true;
    } else {
      session_->stop_on_release = true;
    }
  }
}

void VinputEngine::activateVoiceTrigger(fcitx::InputContext* ic, const fcitx::Key& trigger,
                                        bool is_command) {
  if (recording_cancel_requested_) {
    return;
  }
  dismissMenusForVoiceActivity();
  cancelPendingStop();
  if (session_ && session_->trigger_released) {
    if (session_->phase == Session::Phase::Recording) {
      finishStopRecording();
      return;
    }
    if (session_->phase == Session::Phase::PendingStart) {
      session_->stop_on_release = true;
      return;
    }
  }
  if (session_) {
    ensureStatusSync();
    return;
  }
  const std::string daemon_status = last_known_daemon_status_;
  if (!is_command && daemon_status == vinput::dbus::kStatusRecording) {
    enterRecordingState(ic, trigger, false);
    finishStopRecording();
    return;
  }
  if (!daemon_status.empty() && daemon_status != vinput::dbus::kStatusIdle) {
    applyDaemonStatusLocally(daemon_status, ic, is_command);
    return;
  }
  std::string selected_text;
  if (is_command) {
    // Check command scene has llm_max_candidates > 0 and a valid provider
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
    FCITX_LOG(Debug) << "vinput: command key pressed, selected_text length="
                     << selected_text.size();
  } else {
    FCITX_LOG(Debug) << "vinput: trigger key pressed";
  }
  enterPendingStartState(ic, trigger, is_command);
  const bool started = is_command ? callStartCommandRecording(selected_text) : callStartRecording();
  if (!started) {
    finishFrontendSession(ic);
    const char* action = is_command ? "command" : "record";
    if (!bus_) {
      vinput::debug::Log("%s trigger fallback: daemon bus unavailable\n", action);
      updateVoicePresentation(ic, DaemonUnavailablePreeditText());
    } else if (!daemonSyncAllowed()) {
      vinput::debug::Log("%s trigger fallback: daemon sync throttled after timeout/failure\n",
                         action);
      updateVoicePresentation(ic, DaemonNotRespondingPreeditText());
    }
  }
}

bool VinputEngine::handleCommandPaletteHotkey(fcitx::KeyEvent& keyEvent) {
  // Modifier bindings are handled by the shared gesture recognizer before menus.
  if (keyEvent.key().isModifier() || !keyEvent.key().checkKeyList(menu_keys_)) {
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
  pending_start_ic_ = nullptr;
  if (pending_start_event_ && pending_start_event_->isEnabled()) {
    pending_start_event_->setEnabled(false);
  }
}

void VinputEngine::cancelModifierRecording() {
  if (recording_cancel_requested_ || !session_ || !session_->trigger.isModifier() ||
      !session_->stop_on_release || session_->trigger_released ||
      (session_->phase != Session::Phase::PendingStart &&
       session_->phase != Session::Phase::Recording)) {
    return;
  }
  recording_cancel_requested_ = true;
  session_->trigger_released = true;
  cancelPendingStop();
  clearVoicePresentation(session_->ic);
  // Never cancel another client's recording if our Start request was rejected.
  // A pending Start reply sends cancellation only after confirming success.
  if (!pending_start_call_slot_) {
    callCancelOperation(false);
  }
}

void VinputEngine::scheduleStopRecording() {
  const auto fire_at_usec =
      fcitx::now(CLOCK_MONOTONIC) +
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(kReleaseDebounce).count());

  if (!pending_stop_event_) {
    pending_stop_event_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fire_at_usec, 0, [this](fcitx::EventSourceTime*, uint64_t) {
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
  if (recording_cancel_requested_ || !session_ || session_->phase != Session::Phase::Recording) {
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
