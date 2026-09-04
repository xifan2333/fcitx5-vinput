#include <chrono>
#include <fcitx-utils/key.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <string>

#include "common/config/core_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/debug_log.h"

#include "clipboard_public.h"
#include "core/vinput.h"

namespace {

constexpr auto kReleaseDebounce = std::chrono::milliseconds(500);
constexpr auto kToggleThreshold = std::chrono::milliseconds(300);
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

  if (result_menu_visible_ && handleResultMenuKeyEvent(keyEvent)) {
    return;
  }

  if (scene_menu_visible_ && handleSceneMenuKeyEvent(keyEvent)) {
    return;
  }

  if (asr_menu_visible_ && handleAsrMenuKeyEvent(keyEvent)) {
    return;
  }

  if (!session_ && keyEvent.key().checkKeyList(asr_menu_key_) && !keyEvent.isRelease()) {
    showAsrMenu(keyEvent.inputContext());
    keyEvent.filterAndAccept();
    return;
  }

  if (keyEvent.key().checkKeyList(asr_menu_key_) && keyEvent.isRelease()) {
    keyEvent.filterAndAccept();
    return;
  }

  if (!session_ && keyEvent.key().checkKeyList(scene_menu_key_) && !keyEvent.isRelease()) {
    showSceneMenu(keyEvent.inputContext());
    keyEvent.filterAndAccept();
    return;
  }

  if (keyEvent.key().checkKeyList(scene_menu_key_) && keyEvent.isRelease()) {
    keyEvent.filterAndAccept();
    return;
  }

  const int trigger_index = keyEvent.key().keyListIndex(trigger_keys_);
  const bool is_trigger = trigger_index >= 0;

  const int command_index = keyEvent.key().keyListIndex(command_keys_);
  const bool is_command = command_index >= 0;

  FCITX_LOG(Debug) << "vinput handleKeyEvent: " << keyEvent.key()
                   << " is_release=" << keyEvent.isRelease() << " is_trigger=" << is_trigger
                   << " is_command=" << is_command;

  if ((is_trigger || is_command) && !keyEvent.isRelease()) {
    auto now = std::chrono::steady_clock::now();
    const auto since_last = now - last_trigger_time_;
    last_trigger_time_ = now;
    if (since_last < kTriggerDebounce) {
      keyEvent.filterAndAccept();
      return;
    }

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

    auto* ic = keyEvent.inputContext();
    auto trigger = is_trigger ? trigger_keys_[trigger_index] : command_keys_[command_index];

    // Hold mode: defer start until key held >= kToggleThreshold
    if (trigger_mode_ == TriggerMode::Hold) {
      const std::string daemon_status = last_known_daemon_status_;
      if (is_trigger && !session_ && daemon_status == vinput::dbus::kStatusRecording) {
        hideResultMenu();
        enterRecordingState(ic, trigger, false);
        finishStopRecording();
        keyEvent.filterAndAccept();
        return;
      }
      if (!daemon_status.empty() && daemon_status != vinput::dbus::kStatusIdle) {
        applyDaemonStatusLocally(daemon_status, ic, is_command);
        keyEvent.filterAndAccept();
        return;
      }

      cancelPendingStart();
      const auto fire_at_usec =
          fcitx::now(CLOCK_MONOTONIC) +
          static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(kToggleThreshold).count());
      pending_start_event_ = instance_->eventLoop().addTimeEvent(
          CLOCK_MONOTONIC, fire_at_usec, 0,
          [this, ic, trigger, is_command](fcitx::EventSourceTime*, uint64_t) {
            hideResultMenu();

            if (is_command) {
              {
                auto core_config = LoadCoreConfig();
                const auto* cmd_scene = FindCommandScene(core_config);
                if (!cmd_scene || cmd_scene->llm_max_candidates <= 0) {
                  finishFrontendSession(ic);
                  updatePreedit(ic, CommandDisabledPreeditText());
                  pending_start_event_.reset();
                  return false;
                }
                if (cmd_scene->provider_id.empty() ||
                    ResolveLlmProvider(core_config, cmd_scene->provider_id) == nullptr) {
                  finishFrontendSession(ic);
                  updatePreedit(ic, CommandNoProviderPreeditText());
                  pending_start_event_.reset();
                  return false;
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
                  auto byte_len = fcitx::utf8::ncharByteLength(std::next(text.begin(), byte_from),
                                                               char_to - char_from);
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
                  clearPreedit(ic);
                }
                vinput::debug::Log("command trigger ignored because no selection text is "
                                   "available\n");
                updatePreedit(ic, NoSelectionPreeditText());
                pending_start_event_.reset();
                return false;
              }
              enterPendingStartState(ic, trigger, true);
              FCITX_LOG(Debug) << "vinput: command key held, selected_text length="
                               << selected_text.size();
              if (!callStartCommandRecording(selected_text)) {
                finishFrontendSession(ic);
                if (!bus_) {
                  vinput::debug::Log("command trigger fallback: daemon bus unavailable\n");
                  updatePreedit(ic, DaemonUnavailablePreeditText());
                } else if (!daemonSyncAllowed()) {
                  vinput::debug::Log("command trigger fallback: daemon sync throttled "
                                     "after timeout/failure\n");
                  updatePreedit(ic, DaemonNotRespondingPreeditText());
                }
              }
            } else {
              enterPendingStartState(ic, trigger, false);
              FCITX_LOG(Debug) << "vinput: trigger key held";
              if (!callStartRecording()) {
                finishFrontendSession(ic);
                if (!bus_) {
                  vinput::debug::Log("record trigger fallback: daemon bus unavailable\n");
                  updatePreedit(ic, DaemonUnavailablePreeditText());
                } else if (!daemonSyncAllowed()) {
                  vinput::debug::Log("record trigger fallback: daemon sync throttled "
                                     "after timeout/failure\n");
                  updatePreedit(ic, DaemonNotRespondingPreeditText());
                }
              }
            }
            pending_start_event_.reset();
            return false;
          });
      pending_start_event_->setOneShot();
      keyEvent.filterAndAccept();
      return;
    }

    // Tap / Both mode: start immediately on press
    const std::string daemon_status = last_known_daemon_status_;
    if (is_trigger && !session_ && daemon_status == vinput::dbus::kStatusRecording) {
      hideResultMenu();
      enterRecordingState(ic, trigger, false);
      finishStopRecording();
      keyEvent.filterAndAccept();
      return;
    }
    if (!daemon_status.empty() && daemon_status != vinput::dbus::kStatusIdle) {
      applyDaemonStatusLocally(daemon_status, ic, is_command);
      keyEvent.filterAndAccept();
      return;
    }
    hideResultMenu();

    if (is_command) {
      // Check command scene has llm_max_candidates > 0 and a valid provider
      {
        auto core_config = LoadCoreConfig();
        const auto* cmd_scene = FindCommandScene(core_config);
        if (!cmd_scene || cmd_scene->llm_max_candidates <= 0) {
          finishFrontendSession(ic);
          updatePreedit(ic, CommandDisabledPreeditText());
          keyEvent.filterAndAccept();
          return;
        }
        if (cmd_scene->provider_id.empty() ||
            ResolveLlmProvider(core_config, cmd_scene->provider_id) == nullptr) {
          finishFrontendSession(ic);
          updatePreedit(ic, CommandNoProviderPreeditText());
          keyEvent.filterAndAccept();
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
          clearPreedit(ic);
        }
        vinput::debug::Log("command trigger ignored because no selection text is available\n");
        updatePreedit(ic, NoSelectionPreeditText());
        keyEvent.filterAndAccept();
        return;
      }
      enterPendingStartState(ic, trigger, true);
      FCITX_LOG(Debug) << "vinput: command key pressed, selected_text length="
                       << selected_text.size();
      if (!callStartCommandRecording(selected_text)) {
        finishFrontendSession(ic);
        if (!bus_) {
          vinput::debug::Log("command trigger fallback: daemon bus unavailable\n");
          updatePreedit(ic, DaemonUnavailablePreeditText());
        } else if (!daemonSyncAllowed()) {
          vinput::debug::Log("command trigger fallback: daemon sync throttled "
                             "after timeout/failure\n");
          updatePreedit(ic, DaemonNotRespondingPreeditText());
        }
      }
    } else {
      enterPendingStartState(ic, trigger, false);
      FCITX_LOG(Debug) << "vinput: trigger key pressed";
      if (!callStartRecording()) {
        finishFrontendSession(ic);
        if (!bus_) {
          vinput::debug::Log("record trigger fallback: daemon bus unavailable\n");
          updatePreedit(ic, DaemonUnavailablePreeditText());
        } else if (!daemonSyncAllowed()) {
          vinput::debug::Log("record trigger fallback: daemon sync throttled "
                             "after timeout/failure\n");
          updatePreedit(ic, DaemonNotRespondingPreeditText());
        }
      }
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
    if (session_ && session_->phase == Session::Phase::Recording &&
        isReleaseOfActiveTrigger(keyEvent.key())) {
      session_->trigger_released = true;
      auto held = std::chrono::steady_clock::now() - session_->press_time;
      if (held >= kToggleThreshold) {
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
      isReleaseOfActiveTrigger(keyEvent.key())) {
    session_->trigger_released = true;
    auto held = std::chrono::steady_clock::now() - session_->press_time;
    if (held >= kToggleThreshold) {
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
  if (!session_.has_value() || session_->phase != Session::Phase::Recording) {
    return;
  }

  reloadSceneConfig();
  if (!session_.has_value()) {
    return;
  }
  const auto& scene = vinput::scene::Resolve(scene_config_, active_scene_id_);
  active_scene_id_ = scene.id;
  session_->phase = Session::Phase::Busy;
  session_->trigger = fcitx::Key();
  enterBusyState(session_->ic, session_->command_mode, _("... Recognizing ..."));
  callStopRecording(scene.id);
}
