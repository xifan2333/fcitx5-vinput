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
constexpr auto kHoldThreshold = std::chrono::milliseconds(300);

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

  // 3. Classify trigger keys with dictation/command priority over menu keys
  const int trigger_index = event_key.keyListIndex(trigger_keys_);
  const bool is_trigger = trigger_index >= 0;
  const int command_index = event_key.keyListIndex(command_keys_);
  const bool is_command = !is_trigger && command_index >= 0;
  const bool is_menu = !is_trigger && !is_command && event_key.checkKeyList(menu_keys_);

  // 4. If a non-trigger key is pressed while waiting for trigger release or holding
  if (!is_trigger && !is_command && !is_menu) {
    if (!keyEvent.isRelease()) {
      // If a trigger key is currently held down, an intervening key press means a shortcut
      // combination
      if (active_trigger_key_.has_value()) {
        trigger_interrupted_ = true;
        cancelPendingStart();

        // If hold-recording is already active, discard this recording turn
        if (session_ && session_->stop_on_release && !session_->trigger_released) {
          auto* target_ic = session_->ic;
          callCancelOperation(false);
          finishFrontendSession(target_ic);
          clearVoicePresentation(target_ic);
        }
      }
    }
    // Pass the non-trigger key untouched to the client application
    return;
  }

  // 5. Handle Command Palette Hotkey (menu_keys_, e.g. Shift_R)
  if (is_menu) {
    if (!keyEvent.isRelease()) {
      if (!session_) {
        toggleCommandPalette(ic);
      }
    }
    keyEvent.filterAndAccept();
    return;
  }

  const auto trigger = is_trigger ? trigger_keys_[trigger_index] : command_keys_[command_index];

  // 6. Press Phase
  if (!keyEvent.isRelease()) {
    auto now = std::chrono::steady_clock::now();
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
      if (session_->trigger == trigger) {
        if (session_->trigger_released) {
          // Tap toggle: second press stops recording
          finishStopRecording();
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

    // Ignore auto-repeat while waiting for hold threshold
    if (isPendingStartTrigger(trigger) && pending_start_ic_.get() == ic) {
      keyEvent.filterAndAccept();
      return;
    }

    // New press: arm hold timer and wait for release or hold threshold
    cancelPendingStart();
    active_trigger_key_ = trigger;
    trigger_interrupted_ = false;
    pending_start_trigger_ = trigger;
    pending_start_ic_ =
        ic != nullptr ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();

    const auto fire_at_usec =
        fcitx::now(kDefaultClock) +
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(kHoldThreshold).count());

    pending_start_event_ = instance_->eventLoop().addTimeEvent(
        kDefaultClock, fire_at_usec, 0, [this, trigger, is_command](auto*, uint64_t) {
          auto* target_ic = pending_start_ic_.get();
          if (target_ic == nullptr) {
            cancelPendingStart();
            return false;
          }
          // Held past threshold: start hold-to-talk recording
          startVoiceRecording(target_ic, trigger, is_command);
          if (session_) {
            session_->press_time = std::chrono::steady_clock::now();
            session_->stop_on_release = true;
            session_->trigger_released = false;
          }
          cancelPendingStart();
          return false;
        });
    pending_start_event_->setOneShot();

    keyEvent.filterAndAccept();
    return;
  }

  // 7. Release Phase
  if (keyEvent.isRelease()) {
    const bool matches_active = active_trigger_key_.has_value() && *active_trigger_key_ == trigger;

    if (matches_active) {
      active_trigger_key_.reset();

      // If interrupted by another key (e.g. Alt+Tab, Alt+T), release does NOT trigger voice
      if (trigger_interrupted_) {
        trigger_interrupted_ = false;
        cancelPendingStart();
        keyEvent.filterAndAccept();
        return;
      }

      // If released before hold timer fired, this is a clean tap!
      if (pending_start_event_ && pending_start_event_->isEnabled()) {
        cancelPendingStart();
        // Tap toggle: start continuous recording session
        startVoiceRecording(ic, trigger, is_command);
        if (session_) {
          session_->press_time = std::chrono::steady_clock::now();
          session_->trigger_released = true;
          session_->stop_on_release = false;
        }
        keyEvent.filterAndAccept();
        return;
      }
    }

    // Release during active recording
    if (session_ && session_->trigger == trigger) {
      session_->trigger_released = true;
      if (session_->stop_on_release) {
        // Hold recording: stop on release
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
