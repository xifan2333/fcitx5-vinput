#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fcitx-utils/event.h>
#if __has_include(<fcitx-utils/eventloopinterface.h>)
#include <fcitx-utils/eventloopinterface.h>
#endif
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "common/config/core_config.h"
#include "common/config/core_config_view.h"
#include "common/config/vinput_config.h"
#include "common/dbus/dbus_interface.h"
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

std::string ExtractSurroundingSelection(fcitx::InputContext* ic) {
  if (ic == nullptr) {
    return {};
  }
  auto& surrounding = ic->surroundingText();
  if (!surrounding.isValid() || surrounding.cursor() == surrounding.anchor()) {
    return {};
  }
  const auto& text = surrounding.text();
  if (!fcitx::utf8::validate(text)) {
    return {};
  }
  const auto char_from = std::min(surrounding.cursor(), surrounding.anchor());
  const auto char_to = std::max(surrounding.cursor(), surrounding.anchor());
  const auto byte_from = fcitx::utf8::ncharByteLength(text.begin(), char_from);
  const auto byte_len =
      fcitx::utf8::ncharByteLength(std::next(text.begin(), byte_from), char_to - char_from);
  return text.substr(byte_from, byte_len);
}

std::string ExtractClipboardPrimary(fcitx::Instance* instance, fcitx::InputContext* ic) {
  if (instance == nullptr || ic == nullptr) {
    return {};
  }
  auto* clipboard = instance->addonManager().addon("clipboard");
  if (clipboard == nullptr) {
    return {};
  }
  auto primary = clipboard->call<fcitx::IClipboard::primary>(ic);
  if (!fcitx::utf8::validate(primary)) {
    return {};
  }
  return primary;
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

bool VinputEngine::handleDaemonRecordingPrecondition(fcitx::InputContext* ic,
                                                     const fcitx::Key& trigger, bool is_command) {
  const std::string daemon_status = last_known_daemon_status_;
  if (!is_command && !session_ && daemon_status == vinput::dbus::kStatusRecording) {
    enterRecordingState(ic, trigger, false);
    finishStopRecording();
    return true;
  }
  if (!daemon_status.empty() && daemon_status != vinput::dbus::kStatusIdle) {
    applyDaemonStatusLocally(daemon_status, ic, is_command);
    return true;
  }
  return false;
}

bool VinputEngine::validateCommandModePrerequisites(fcitx::InputContext* ic) {
  auto core_config = LoadCoreConfig();
  const CoreConfigView view(core_config);
  const auto* cmd_scene = view.CommandScene();
  if (cmd_scene == nullptr || cmd_scene->llm_max_candidates <= 0) {
    finishFrontendSession(ic);
    updateVoicePresentation(ic, CommandDisabledPreeditText());
    return false;
  }
  if (cmd_scene->provider_id.empty() || (view.LlmProvider(cmd_scene->provider_id) == nullptr &&
                                         view.LlmAdapter(cmd_scene->provider_id) == nullptr)) {
    finishFrontendSession(ic);
    updateVoicePresentation(ic, CommandNoProviderPreeditText());
    return false;
  }
  return true;
}

std::string VinputEngine::acquireSelectionText(fcitx::InputContext* ic) {
  std::string selected_text = ExtractSurroundingSelection(ic);
  if (selected_text.empty()) {
    selected_text = ExtractClipboardPrimary(instance_, ic);
  }
  return selected_text;
}

void VinputEngine::handleRecordingStartFailure(fcitx::InputContext* ic, bool is_command) {
  finishFrontendSession(ic);
  if (bus_ == nullptr) {
    VLOG_DEBUG("{} trigger fallback: daemon bus unavailable", is_command ? "command" : "record");
    updateVoicePresentation(ic, DaemonUnavailablePreeditText());
    return;
  }
  if (!daemonSyncAllowed()) {
    VLOG_DEBUG("{} trigger fallback: daemon sync throttled after timeout/failure",
               is_command ? "command" : "record");
    updateVoicePresentation(ic, DaemonNotRespondingPreeditText());
  }
}

void VinputEngine::dispatchRecordingStart(fcitx::InputContext* ic, const fcitx::Key& trigger,
                                          bool is_command, const std::string& selected_text) {
  enterPendingStartState(ic, trigger, is_command);
  if (is_command) {
    FCITX_LOG(Debug) << "vinput: command key activated, selected_text length="
                     << selected_text.size();
    if (!callStartCommandRecording(selected_text)) {
      handleRecordingStartFailure(ic, true);
    }
    return;
  }

  FCITX_LOG(Debug) << "vinput: trigger key activated";
  if (!callStartRecording()) {
    handleRecordingStartFailure(ic, false);
  }
}

void VinputEngine::startVoiceRecording(fcitx::InputContext* ic, const fcitx::Key& trigger,
                                       bool is_command) {
  if (ic == nullptr) {
    return;
  }
  dismissMenusForVoiceActivity();
  cancelPendingStop();

  if (handleDaemonRecordingPrecondition(ic, trigger, is_command)) {
    return;
  }

  std::string selected_text;
  if (is_command) {
    if (!validateCommandModePrerequisites(ic)) {
      return;
    }
    selected_text = acquireSelectionText(ic);
    if (selected_text.empty()) {
      if (status_ic_ == ic) {
        finishFrontendSession(ic);
      } else {
        clearVoicePresentation(ic);
      }
      VLOG_DEBUG("command trigger ignored because no selection text is available");
      updateVoicePresentation(ic, NoSelectionPreeditText());
      return;
    }
  }

  dispatchRecordingStart(ic, trigger, is_command, selected_text);
}

bool VinputEngine::handlePostprocessingKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (pending_postprocessing_release_ && keyEvent.isRelease() &&
      keyEvent.key().normalize().sym() == pending_postprocessing_release_->normalize().sym()) {
    pending_postprocessing_release_.reset();
    keyEvent.filterAndAccept();
    return true;
  }

  if (!session_ || session_->phase != Session::Phase::Postprocessing ||
      last_known_daemon_status_ != vinput::dbus::kStatusPostprocessing) {
    return false;
  }

  const bool discard = keyEvent.key().check(FcitxKey_Escape);
  const bool commit_raw =
      (session_->raw_prev && !session_->command_mode) &&
      (keyEvent.key().check(FcitxKey_Return) || keyEvent.key().check(FcitxKey_KP_Enter));

  if (!discard && !commit_raw) {
    return false;
  }

  if (!keyEvent.isRelease()) {
    pending_postprocessing_release_ = keyEvent.key();
    callCancelOperation(commit_raw);
  }
  keyEvent.filterAndAccept();
  return true;
}

bool VinputEngine::handleMenuNavigationKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (result_menu_visible_ && handleResultMenuKeyEvent(keyEvent)) {
    return true;
  }
  if (palette_menu_visible_ && handlePaletteMenuKeyEvent(keyEvent)) {
    return true;
  }
  return false;
}

VinputEngine::HotkeyMatch VinputEngine::classifyHotkey(const fcitx::Key& event_key,
                                                       bool is_release) const {
  const int trigger_index = matchKeyListIndex(event_key, trigger_keys_, is_release);
  if (trigger_index >= 0) {
    return {HotkeyRole::Trigger, trigger_keys_[trigger_index], false};
  }

  const int command_index = matchKeyListIndex(event_key, command_keys_, is_release);
  if (command_index >= 0) {
    return {HotkeyRole::Command, command_keys_[command_index], true};
  }

  const int menu_index = matchKeyListIndex(event_key, menu_keys_, is_release);
  if (menu_index >= 0) {
    return {HotkeyRole::Menu, menu_keys_[menu_index], false};
  }

  return {HotkeyRole::None, fcitx::Key(), false};
}

void VinputEngine::handleNonHotkeyKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (keyEvent.isRelease() || held_role_ == HotkeyRole::None) {
    return;
  }

  chord_interrupted_ = true;
  cancelPendingStart();

  if (session_ && session_->stop_on_release && !session_->trigger_released) {
    auto* target_ic = session_->ic;
    callCancelOperation(false);
    finishFrontendSession(target_ic);
    clearVoicePresentation(target_ic);
  }
}

bool VinputEngine::handleActiveSessionHotkeyDown(const fcitx::Key& matched_key) {
  if (!session_ || (session_->phase != Session::Phase::Recording &&
                    session_->phase != Session::Phase::PendingStart)) {
    return false;
  }

  if (session_->trigger != matched_key) {
    auto* target_ic = session_->ic;
    callCancelOperation(false);
    finishFrontendSession(target_ic);
    clearVoicePresentation(target_ic);
    return true;
  }

  if (session_->trigger_released) {
    if (session_->phase == Session::Phase::PendingStart) {
      auto* target_ic = session_->ic;
      callCancelOperation(false);
      finishFrontendSession(target_ic);
      clearVoicePresentation(target_ic);
    } else {
      finishStopRecording();
    }
  }
  return true;
}

void VinputEngine::handleHotkeyDown(fcitx::KeyEvent& keyEvent, fcitx::InputContext* ic,
                                    const HotkeyMatch& match) {
  const auto now = std::chrono::steady_clock::now();

  if (match.role == HotkeyRole::Trigger || match.role == HotkeyRole::Command) {
    const auto since_last = now - last_trigger_time_;
    last_trigger_time_ = now;
    if (since_last < kTriggerDebounce) {
      keyEvent.filterAndAccept();
      return;
    }

    dismissMenusForVoiceActivity();
    cancelPendingStop();

    if (handleActiveSessionHotkeyDown(match.matched_key)) {
      keyEvent.filterAndAccept();
      return;
    }
  }

  held_key_sym_ = keyEvent.origKey().normalize().sym();
  held_role_ = match.role;
  chord_interrupted_ = false;
  held_press_time_ = now;

  if (match.role == HotkeyRole::Trigger || match.role == HotkeyRole::Command) {
    const auto trigger_mode = *config_.triggerMode;
    if (trigger_mode == TriggerMode::Hold || trigger_mode == TriggerMode::Both) {
      startVoiceRecording(ic, match.matched_key, match.is_command);
      if (session_) {
        session_->press_time = now;
        session_->stop_on_release = true;
        session_->trigger_released = false;
      }
    }
  }

  keyEvent.filterAndAccept();
}

void VinputEngine::handleTapReleaseMode(fcitx::InputContext* ic, const HotkeyMatch& match,
                                        std::chrono::steady_clock::time_point now) {
  if (!session_) {
    startVoiceRecording(ic, match.matched_key, match.is_command);
    if (session_) {
      session_->press_time = now;
      session_->trigger_released = true;
      session_->stop_on_release = false;
    }
    return;
  }
  if (session_->phase == Session::Phase::Recording) {
    scheduleStopRecording();
  }
}

void VinputEngine::handleTriggerReleaseMode(fcitx::InputContext* ic, const HotkeyMatch& match,
                                            std::chrono::steady_clock::duration duration,
                                            std::chrono::steady_clock::time_point now) {
  const auto trigger_mode = *config_.triggerMode;
  if (trigger_mode == TriggerMode::Hold) {
    if (session_ && session_->phase == Session::Phase::Recording) {
      scheduleStopRecording();
    }
    return;
  }

  if (trigger_mode == TriggerMode::Tap) {
    handleTapReleaseMode(ic, match, now);
    return;
  }

  // TriggerMode::Both
  if (duration < kTapMaxHoldDuration) {
    if (session_) {
      session_->trigger_released = true;
      session_->stop_on_release = false;
    }
    return;
  }

  if (session_ && session_->phase == Session::Phase::Recording) {
    scheduleStopRecording();
  }
}

void VinputEngine::handleActiveHeldHotkeyRelease(fcitx::InputContext* ic, const HotkeyMatch& match,
                                                 HotkeyRole active_role) {
  if (active_role == HotkeyRole::Menu) {
    if (!session_) {
      toggleCommandPalette(ic);
    }
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  handleTriggerReleaseMode(ic, match, now - held_press_time_, now);
}

void VinputEngine::handleHotkeyUp(fcitx::KeyEvent& keyEvent, fcitx::InputContext* ic,
                                  const HotkeyMatch& match) {
  const auto event_key = keyEvent.origKey().normalize();
  const bool matches_active =
      (held_role_ == match.role && held_key_sym_.has_value() && *held_key_sym_ == event_key.sym());

  if (matches_active) {
    const auto active_role = held_role_;
    held_role_ = HotkeyRole::None;
    held_key_sym_.reset();

    if (chord_interrupted_) {
      chord_interrupted_ = false;
      cancelPendingStart();
      keyEvent.filterAndAccept();
      return;
    }

    handleActiveHeldHotkeyRelease(ic, match, active_role);
  }

  if (session_ && session_->trigger == match.matched_key) {
    session_->trigger_released = true;
    if (session_->stop_on_release && session_->phase == Session::Phase::Recording) {
      scheduleStopRecording();
    }
  }

  keyEvent.filterAndAccept();
}

void VinputEngine::handleKeyEvent(fcitx::Event& event) {
  auto& keyEvent = dynamic_cast<fcitx::KeyEvent&>(event);
  auto* ic = keyEvent.inputContext();
  rememberInputContext(ic);

  if (handlePostprocessingKeyEvent(keyEvent)) {
    return;
  }
  if (handleMenuNavigationKeyEvent(keyEvent)) {
    return;
  }

  const auto event_key = keyEvent.origKey().normalize();
  const auto hotkey_match = classifyHotkey(event_key, keyEvent.isRelease());

  if (hotkey_match.role == HotkeyRole::None) {
    handleNonHotkeyKeyEvent(keyEvent);
    return;
  }

  if (!keyEvent.isRelease()) {
    handleHotkeyDown(keyEvent, ic, hotkey_match);
    return;
  }

  handleHotkeyUp(keyEvent, ic, hotkey_match);
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
    const std::weak_ptr<bool> token = lifetime_token_;
    pending_stop_event_ = instance_->eventLoop().addTimeEvent(kDefaultClock, fire_at_usec, 0,
                                                              [this, token](auto*, uint64_t) {
                                                                if (token.expired()) {
                                                                  return false;
                                                                }
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
