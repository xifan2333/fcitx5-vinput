#include "vinput.h"
#include "common/dbus_interface.h"
#include "common/postprocess_scene.h"

#include <dbus_public.h>
#include <fcitx-utils/dbus/matchrule.h>
#include <fcitx-utils/dbus/message.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>

#include <chrono>
#include <cstdio>
#include <string>

using namespace vinput::dbus;

namespace {

constexpr const char *kSystemdBusName = "org.freedesktop.systemd1";
constexpr const char *kSystemdPath = "/org/freedesktop/systemd1";
constexpr const char *kSystemdManagerInterface =
    "org.freedesktop.systemd1.Manager";
constexpr const char *kSystemdRestartUnit = "RestartUnit";
constexpr uint64_t kSystemdCallTimeoutUsec = 5 * 1000 * 1000;
constexpr const char *kDaemonUnitName = "vinput-daemon.service";
constexpr const char *kReplaceMode = "replace";
constexpr auto kReleaseDebounce = std::chrono::milliseconds(40);
constexpr int kMenuPageSize = 10;

struct SceneOption {
  std::size_t index;
  std::string label;
};

bool UseChineseSceneUi() { return UseChineseUi(); }

std::string SceneMenuTitle() {
  return UseChineseSceneUi() ? "选择后处理选单" : "Choose Postprocess Menu";
}

std::string ResultMenuTitle(std::size_t count) {
  if (UseChineseSceneUi()) {
    return "选择结果（" + std::to_string(count) + "项）";
  }
  return "Choose Result (" + std::to_string(count) + ")";
}

std::string RecordingPreeditText() {
  return UseChineseSceneUi() ? "... 正在录音 ..." : "... Recording ...";
}

std::string InferringPreeditText() {
  return UseChineseSceneUi() ? "... 正在识别 ..." : "... Recognizing ...";
}

std::string PostprocessingPreeditText() {
  return UseChineseSceneUi() ? "... 正在后处理 ..." : "... Postprocessing ...";
}

std::string ResultCandidateComment(const vinput::result::Candidate &candidate,
                                   std::size_t llm_index) {
  if (candidate.source == vinput::result::kSourceRaw) {
    return UseChineseSceneUi() ? "原始识别" : "Raw ASR";
  }
  if (UseChineseSceneUi()) {
    return "候选 " + std::to_string(llm_index);
  }
  return "Option " + std::to_string(llm_index);
}

std::string DisplayCandidateText(std::string text) {
  for (char &ch : text) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      ch = ' ';
    }
  }
  return text;
}

bool MatchesKeyList(const fcitx::Key &key, const fcitx::KeyList &key_list) {
  return key.keyListIndex(key_list) >= 0;
}

std::string DecoratePagedMenuTitle(const std::string &base_title,
                                   fcitx::CandidateList *candidate_list) {
  auto *pageable = candidate_list ? candidate_list->toPageable() : nullptr;
  if (!pageable) {
    return base_title;
  }

  const int total_pages = pageable->totalPages();
  const int current_page = pageable->currentPage();
  if (total_pages <= 1 || current_page < 0) {
    return base_title;
  }

  if (UseChineseSceneUi()) {
    return base_title + "（" + std::to_string(current_page + 1) + "/" +
           std::to_string(total_pages) + "页）";
  }
  return base_title + " (" + std::to_string(current_page + 1) + "/" +
         std::to_string(total_pages) + ")";
}

void SetMenuTitle(fcitx::InputContext *ic, const std::string &base_title,
                  fcitx::CandidateList *candidate_list) {
  if (!ic) {
    return;
  }

  fcitx::Text aux_up;
  aux_up.append(DecoratePagedMenuTitle(base_title, candidate_list));
  ic->inputPanel().setAuxUp(aux_up);
}

int DigitSelectionIndex(fcitx::CandidateList *candidate_list, int digit) {
  auto *pageable = candidate_list ? candidate_list->toPageable() : nullptr;
  int current_page = pageable ? pageable->currentPage() : 0;
  if (current_page < 0) {
    current_page = 0;
  }
  return current_page * kMenuPageSize + digit;
}

int CurrentSelectionIndex(fcitx::CandidateList *candidate_list) {
  if (!candidate_list) {
    return -1;
  }

  int current_index = candidate_list->cursorIndex();
  if (current_index < 0) {
    return -1;
  }

  auto *pageable = candidate_list->toPageable();
  int current_page = pageable ? pageable->currentPage() : 0;
  if (current_page < 0) {
    current_page = 0;
  }

  return current_page * kMenuPageSize + current_index;
}

void MoveCursorToIndex(fcitx::CandidateList *candidate_list, int target_index) {
  auto *cursor_list =
      candidate_list ? candidate_list->toCursorMovable() : nullptr;
  if (!cursor_list || target_index <= 0) {
    return;
  }

  for (int i = 0; i < target_index; ++i) {
    cursor_list->nextCandidate();
  }
}

std::string DisplayTextWithComment(std::string text,
                                   const std::string &comment) {
  if (comment.empty()) {
    return text;
  }
  text.append(" ");
  text.append(comment);
  return text;
}

bool ChangeCandidatePage(fcitx::InputContext *ic, const std::string &base_title,
                         bool next_page) {
  if (!ic) {
    return false;
  }

  auto candidate_list = ic->inputPanel().candidateList();
  auto *pageable = candidate_list ? candidate_list->toPageable() : nullptr;
  if (!pageable) {
    return false;
  }

  if (next_page) {
    if (!pageable->hasNext()) {
      return false;
    }
    pageable->next();
  } else {
    if (!pageable->hasPrev()) {
      return false;
    }
    pageable->prev();
  }

  SetMenuTitle(ic, base_title, candidate_list.get());
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  return true;
}

class SceneCandidateWord : public fcitx::CandidateWord {
public:
  SceneCandidateWord(VinputEngine *engine, SceneOption option, bool active)
      : fcitx::CandidateWord(fcitx::Text(DisplayTextWithComment(
            option.label, active
                              ? (UseChineseSceneUi() ? "（当前）" : "(Current)")
                              : std::string()))),
        engine_(engine), index_(option.index) {}

  void select(fcitx::InputContext *inputContext) const override {
    engine_->selectScene(index_, inputContext);
  }

private:
  VinputEngine *engine_;
  std::size_t index_;
};

class ResultCandidateWord : public fcitx::CandidateWord {
public:
  ResultCandidateWord(VinputEngine *engine, std::size_t index,
                      const std::string &text, const std::string &comment)
      : fcitx::CandidateWord(fcitx::Text(
            DisplayTextWithComment(DisplayCandidateText(text), comment))),
        engine_(engine), index_(index) {}

  void select(fcitx::InputContext *inputContext) const override {
    engine_->selectResultCandidate(index_, inputContext);
  }

private:
  VinputEngine *engine_;
  std::size_t index_;
};

} // namespace

VinputEngine::VinputEngine(fcitx::Instance *instance) : instance_(instance) {
  reloadConfig();

  key_event_handler_ = instance_->watchEvent(
      fcitx::EventType::InputContextKeyEvent,
      fcitx::EventWatcherPhase::PreInputMethod,
      [this](fcitx::Event &event) { handleKeyEvent(event); });

  auto *dbus_addon = instance_->addonManager().addon("dbus");
  if (dbus_addon) {
    bus_ = dbus_addon->call<fcitx::IDBusModule::bus>();
    setupDBusWatcher();
  }
}

VinputEngine::~VinputEngine() = default;

void VinputEngine::setupDBusWatcher() {
  if (!bus_)
    return;

  fcitx::dbus::MatchRule result_rule(kBusName, kObjectPath, kInterface,
                                     kSignalRecognitionResult);

  result_slot_ = bus_->addMatch(result_rule, [this](fcitx::dbus::Message &msg) {
    onRecognitionResult(msg);
    return true;
  });

  fcitx::dbus::MatchRule status_rule(kBusName, kObjectPath, kInterface,
                                     kSignalStatusChanged);

  status_slot_ = bus_->addMatch(status_rule, [this](fcitx::dbus::Message &msg) {
    onStatusChanged(msg);
    return true;
  });
}

void VinputEngine::handleKeyEvent(fcitx::Event &event) {
  auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);

  if (result_menu_visible_ && handleResultMenuKeyEvent(keyEvent)) {
    return;
  }

  if (scene_menu_visible_ && handleSceneMenuKeyEvent(keyEvent)) {
    return;
  }

  if (!recording_ && MatchesKeyList(keyEvent.key(), scene_menu_key_) &&
      !keyEvent.isRelease()) {
    showSceneMenu(keyEvent.inputContext());
    keyEvent.filterAndAccept();
    return;
  }

  if (MatchesKeyList(keyEvent.key(), scene_menu_key_) && keyEvent.isRelease()) {
    keyEvent.filterAndAccept();
    return;
  }

  const int trigger_index = keyEvent.key().keyListIndex(trigger_keys_);
  const bool is_trigger = trigger_index >= 0;

  if (is_trigger && !keyEvent.isRelease()) {
    cancelPendingStop();
    if (!recording_) {
      recording_ = true;
      active_trigger_ = trigger_keys_[trigger_index];
      active_ic_ = keyEvent.inputContext();
      hideResultMenu();
      callStartRecording();
      updatePreedit(active_ic_, RecordingPreeditText());
    }
    keyEvent.filterAndAccept();
    return;
  }

  if (recording_ && keyEvent.isRelease() &&
      isReleaseOfActiveTrigger(keyEvent.key())) {
    scheduleStopRecording();
    keyEvent.filterAndAccept();
    return;
  }

  if (is_trigger && keyEvent.isRelease()) {
    keyEvent.filterAndAccept();
    return;
  }
}

void VinputEngine::reloadConfig() {
  settings_ = LoadVinputSettings();
  applySettings();
  reloadSceneConfig();
  ui_config_.reset();
}

void VinputEngine::save() { SaveVinputSettings(settings_); }

const fcitx::Configuration *VinputEngine::getConfig() const {
  rebuildUiConfig();
  return ui_config_.get();
}

void VinputEngine::setConfig(const fcitx::RawConfig &rawConfig) {
  const auto old_settings = settings_;
  auto config = BuildVinputConfig(settings_);
  config->load(rawConfig, true);
  settings_ = config->settings();
  applySettings();
  SaveVinputSettings(settings_);
  ui_config_ = std::move(config);
}

void VinputEngine::applySettings() {
  trigger_keys_ = settings_.triggerKeys;
  scene_menu_key_ = settings_.sceneMenuKey;
  page_prev_keys_ = settings_.pagePrevKeys;
  page_next_keys_ = settings_.pageNextKeys;
}

void VinputEngine::reloadSceneConfig() {
  scene_config_ = vinput::scene::LoadConfig();
  if (scene_config_.scenes.empty()) {
    scene_config_ = vinput::scene::DefaultConfig();
  }
  active_scene_id_ = vinput::scene::Resolve(scene_config_, active_scene_id_).id;
}

void VinputEngine::rebuildUiConfig() const {
  ui_config_ = BuildVinputConfig(settings_);
}

void VinputEngine::showSceneMenu(fcitx::InputContext *ic) {
  if (!ic) {
    return;
  }

  reloadSceneConfig();
  scene_menu_ic_ = ic;
  scene_menu_visible_ = true;

  auto candidate_list = std::make_unique<fcitx::CommonCandidateList>();
  candidate_list->setPageSize(kMenuPageSize);
  candidate_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
  candidate_list->setCursorPositionAfterPaging(
      fcitx::CursorPositionAfterPaging::ResetToFirst);

  int active_index = 0;
  for (std::size_t i = 0; i < scene_config_.scenes.size(); ++i) {
    const auto &scene = scene_config_.scenes[i];
    const bool active = scene.id == active_scene_id_;
    if (active) {
      active_index = static_cast<int>(i);
    }
    candidate_list->append<SceneCandidateWord>(
        this,
        SceneOption{
            .index = i,
            .label = vinput::scene::DisplayLabel(scene, UseChineseSceneUi()),
        },
        active);
  }
  MoveCursorToIndex(candidate_list.get(), active_index);

  SetMenuTitle(ic, SceneMenuTitle(), candidate_list.get());
  ic->inputPanel().setCandidateList(std::move(candidate_list));
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VinputEngine::hideSceneMenu() {
  if (!scene_menu_visible_ || !scene_menu_ic_) {
    scene_menu_visible_ = false;
    scene_menu_ic_ = nullptr;
    return;
  }

  scene_menu_visible_ = false;
  fcitx::Text empty;
  scene_menu_ic_->inputPanel().setAuxUp(empty);
  scene_menu_ic_->inputPanel().setCandidateList({});
  scene_menu_ic_->updateUserInterface(
      fcitx::UserInterfaceComponent::InputPanel);
  scene_menu_ic_ = nullptr;
}

void VinputEngine::showResultMenu(fcitx::InputContext *ic,
                                  const vinput::result::Payload &payload) {
  if (!ic || payload.candidates.empty()) {
    return;
  }

  hideSceneMenu();
  result_menu_ic_ = ic;
  result_menu_visible_ = true;
  result_candidates_ = payload.candidates;

  auto candidate_list = std::make_unique<fcitx::CommonCandidateList>();
  candidate_list->setPageSize(kMenuPageSize);
  candidate_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
  candidate_list->setCursorPositionAfterPaging(
      fcitx::CursorPositionAfterPaging::ResetToFirst);

  int cursor_index = 0;
  std::size_t llm_index = 0;
  for (std::size_t i = 0; i < result_candidates_.size(); ++i) {
    const auto &candidate = result_candidates_[i];
    if (candidate.source == vinput::result::kSourceLlm) {
      ++llm_index;
    }
    if (candidate.text == payload.commitText) {
      cursor_index = static_cast<int>(i);
    }
    candidate_list->append<ResultCandidateWord>(
        this, i, candidate.text, ResultCandidateComment(candidate, llm_index));
  }
  MoveCursorToIndex(candidate_list.get(), cursor_index);

  SetMenuTitle(ic, ResultMenuTitle(result_candidates_.size()),
               candidate_list.get());
  ic->inputPanel().setCandidateList(std::move(candidate_list));
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VinputEngine::hideResultMenu() {
  if (!result_menu_visible_ || !result_menu_ic_) {
    result_menu_visible_ = false;
    result_menu_ic_ = nullptr;
    result_candidates_.clear();
    return;
  }

  result_menu_visible_ = false;
  fcitx::Text empty;
  result_menu_ic_->inputPanel().setAuxUp(empty);
  result_menu_ic_->inputPanel().setCandidateList({});
  result_menu_ic_->updateUserInterface(
      fcitx::UserInterfaceComponent::InputPanel);
  result_menu_ic_ = nullptr;
  result_candidates_.clear();
}

bool VinputEngine::handleSceneMenuKeyEvent(fcitx::KeyEvent &keyEvent) {
  if (!scene_menu_visible_ || !scene_menu_ic_) {
    return false;
  }

  auto candidate_list = scene_menu_ic_->inputPanel().candidateList();
  auto *cursor_list =
      candidate_list ? candidate_list->toCursorMovable() : nullptr;
  if (keyEvent.isRelease()) {
    if (MatchesKeyList(keyEvent.key(), scene_menu_key_) ||
        MatchesKeyList(keyEvent.key(), page_prev_keys_) ||
        MatchesKeyList(keyEvent.key(), page_next_keys_) ||
        keyEvent.key().digitSelection() >= 0 ||
        keyEvent.key().check(FcitxKey_Up) ||
        keyEvent.key().check(FcitxKey_Down) ||
        keyEvent.key().check(FcitxKey_Return) ||
        keyEvent.key().check(FcitxKey_KP_Enter) ||
        keyEvent.key().check(FcitxKey_Escape)) {
      keyEvent.filterAndAccept();
      return true;
    }
    return false;
  }

  if (MatchesKeyList(keyEvent.key(), scene_menu_key_)) {
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().check(FcitxKey_Escape)) {
    hideSceneMenu();
    keyEvent.filterAndAccept();
    return true;
  }

  if (MatchesKeyList(keyEvent.key(), page_prev_keys_)) {
    ChangeCandidatePage(scene_menu_ic_, SceneMenuTitle(), false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (MatchesKeyList(keyEvent.key(), page_next_keys_)) {
    ChangeCandidatePage(scene_menu_ic_, SceneMenuTitle(), true);
    keyEvent.filterAndAccept();
    return true;
  }

  const int digit = keyEvent.key().digitSelection();
  const int digit_index = DigitSelectionIndex(candidate_list.get(), digit);
  if (digit >= 0 &&
      digit_index < static_cast<int>(scene_config_.scenes.size())) {
    selectScene(static_cast<std::size_t>(digit_index), scene_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && keyEvent.key().check(FcitxKey_Up)) {
    cursor_list->prevCandidate();
    scene_menu_ic_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && keyEvent.key().check(FcitxKey_Down)) {
    cursor_list->nextCandidate();
    scene_menu_ic_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().check(FcitxKey_Return) ||
      keyEvent.key().check(FcitxKey_KP_Enter)) {
    int index = CurrentSelectionIndex(candidate_list.get());
    if (index < 0) {
      for (std::size_t i = 0; i < scene_config_.scenes.size(); ++i) {
        if (scene_config_.scenes[i].id == active_scene_id_) {
          index = static_cast<int>(i);
          break;
        }
      }
    }
    if (index >= 0 && index < static_cast<int>(scene_config_.scenes.size())) {
      selectScene(static_cast<std::size_t>(index), scene_menu_ic_);
    } else {
      hideSceneMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  hideSceneMenu();
  return false;
}

bool VinputEngine::handleResultMenuKeyEvent(fcitx::KeyEvent &keyEvent) {
  if (!result_menu_visible_ || !result_menu_ic_) {
    return false;
  }

  auto candidate_list = result_menu_ic_->inputPanel().candidateList();
  auto *cursor_list =
      candidate_list ? candidate_list->toCursorMovable() : nullptr;
  if (keyEvent.isRelease()) {
    if (keyEvent.key().digitSelection() >= 0 ||
        MatchesKeyList(keyEvent.key(), page_prev_keys_) ||
        MatchesKeyList(keyEvent.key(), page_next_keys_) ||
        keyEvent.key().check(FcitxKey_Up) ||
        keyEvent.key().check(FcitxKey_Down) ||
        keyEvent.key().check(FcitxKey_Return) ||
        keyEvent.key().check(FcitxKey_KP_Enter) ||
        keyEvent.key().check(FcitxKey_Escape)) {
      keyEvent.filterAndAccept();
      return true;
    }
    return false;
  }

  if (keyEvent.key().check(FcitxKey_Escape)) {
    hideResultMenu();
    keyEvent.filterAndAccept();
    return true;
  }

  if (MatchesKeyList(keyEvent.key(), page_prev_keys_)) {
    ChangeCandidatePage(result_menu_ic_,
                        ResultMenuTitle(result_candidates_.size()), false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (MatchesKeyList(keyEvent.key(), page_next_keys_)) {
    ChangeCandidatePage(result_menu_ic_,
                        ResultMenuTitle(result_candidates_.size()), true);
    keyEvent.filterAndAccept();
    return true;
  }

  const int digit = keyEvent.key().digitSelection();
  const int digit_index = DigitSelectionIndex(candidate_list.get(), digit);
  if (digit >= 0 && digit_index < static_cast<int>(result_candidates_.size())) {
    selectResultCandidate(static_cast<std::size_t>(digit_index),
                          result_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && keyEvent.key().check(FcitxKey_Up)) {
    cursor_list->prevCandidate();
    result_menu_ic_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && keyEvent.key().check(FcitxKey_Down)) {
    cursor_list->nextCandidate();
    result_menu_ic_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().check(FcitxKey_Return) ||
      keyEvent.key().check(FcitxKey_KP_Enter)) {
    int index = CurrentSelectionIndex(candidate_list.get());
    if (index < 0) {
      index = 0;
    }
    if (index >= 0 && index < static_cast<int>(result_candidates_.size())) {
      selectResultCandidate(static_cast<std::size_t>(index), result_menu_ic_);
    } else {
      hideResultMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  hideResultMenu();
  return false;
}

void VinputEngine::selectScene(std::size_t index, fcitx::InputContext *ic) {
  if (index >= scene_config_.scenes.size()) {
    hideSceneMenu();
    return;
  }

  active_scene_id_ = scene_config_.scenes[index].id;
  hideSceneMenu();
  (void)ic;
}

void VinputEngine::selectResultCandidate(std::size_t index,
                                         fcitx::InputContext *ic) {
  if (index >= result_candidates_.size()) {
    hideResultMenu();
    return;
  }

  const std::string text = result_candidates_[index].text;
  hideResultMenu();
  if (!text.empty() && ic) {
    clearPreedit(ic);
    ic->commitString(text);
  }
}

bool VinputEngine::isReleaseOfActiveTrigger(const fcitx::Key &key) const {
  if (!active_trigger_.isValid()) {
    return false;
  }

  const auto release_key = key.normalize();
  const auto trigger_key = active_trigger_.normalize();

  if (trigger_key.isModifier() &&
      release_key.isReleaseOfModifier(trigger_key)) {
    return true;
  }

  if (release_key.sym() == trigger_key.sym()) {
    return true;
  }

  const auto released_modifier_state =
      fcitx::Key::keySymToStates(release_key.sym());
  return released_modifier_state.toInteger() != 0 &&
         trigger_key.states().testAny(released_modifier_state);
}

void VinputEngine::cancelPendingStop() {
  if (pending_stop_event_ && pending_stop_event_->isEnabled()) {
    pending_stop_event_->setEnabled(false);
  }
}

void VinputEngine::scheduleStopRecording() {
  const auto fire_at_usec =
      fcitx::now(CLOCK_MONOTONIC) +
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              kReleaseDebounce)
              .count());

  if (!pending_stop_event_) {
    pending_stop_event_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fire_at_usec, 0,
        [this](fcitx::EventSourceTime *, uint64_t) {
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
  if (!recording_) {
    return;
  }

  reloadSceneConfig();
  const auto &scene = vinput::scene::Resolve(scene_config_, active_scene_id_);
  active_scene_id_ = scene.id;
  recording_ = false;
  active_trigger_ = fcitx::Key();
  callStopRecording(scene.id);
  if (active_ic_) {
    updatePreedit(active_ic_, InferringPreeditText());
  }
}

void VinputEngine::restartDaemon() {
  if (!bus_) {
    fprintf(
        stderr,
        "vinput: cannot restart vinput-daemon because DBus is unavailable\n");
    return;
  }

  auto msg =
      bus_->createMethodCall(kSystemdBusName, kSystemdPath,
                             kSystemdManagerInterface, kSystemdRestartUnit);
  msg << kDaemonUnitName << kReplaceMode;

  auto reply = msg.call(kSystemdCallTimeoutUsec);
  if (!reply) {
    fprintf(stderr,
            "vinput: failed to restart vinput-daemon via systemd user bus\n");
    return;
  }

  if (reply.isError()) {
    fprintf(stderr, "vinput: systemd restart failed: %s: %s\n",
            reply.errorName().c_str(), reply.errorMessage().c_str());
  }
}

void VinputEngine::callStartRecording() {
  if (!bus_)
    return;
  auto msg = bus_->createMethodCall(kBusName, kObjectPath, kInterface,
                                    kMethodStartRecording);
  msg.send();
}

void VinputEngine::callStopRecording(const std::string &scene_id) {
  if (!bus_)
    return;
  auto msg = bus_->createMethodCall(kBusName, kObjectPath, kInterface,
                                    kMethodStopRecording);
  msg << scene_id;
  msg.send();
}

void VinputEngine::onRecognitionResult(fcitx::dbus::Message &msg) {
  std::string payload_text;
  msg >> payload_text;

  if (!active_ic_) {
    return;
  }

  hideResultMenu();

  const auto payload = vinput::result::Parse(payload_text);
  clearPreedit(active_ic_);
  if (payload.candidates.size() > 1) {
    showResultMenu(active_ic_, payload);
    return;
  }

  if (!payload.commitText.empty()) {
    active_ic_->commitString(payload.commitText);
  }
}

void VinputEngine::onStatusChanged(fcitx::dbus::Message &msg) {
  std::string status;
  msg >> status;

  if (!active_ic_)
    return;

  if (status == "recording") {
    updatePreedit(active_ic_, RecordingPreeditText());
  } else if (status == "inferring") {
    updatePreedit(active_ic_, InferringPreeditText());
  } else if (status == "postprocessing") {
    updatePreedit(active_ic_, PostprocessingPreeditText());
  } else {
    clearPreedit(active_ic_);
  }
}

void VinputEngine::updatePreedit(fcitx::InputContext *ic,
                                 const std::string &text) {
  if (!ic)
    return;
  fcitx::Text preedit;
  preedit.append(text);
  ic->inputPanel().setPreedit(preedit);
  ic->inputPanel().setClientPreedit(preedit);
  ic->updatePreedit();
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VinputEngine::clearPreedit(fcitx::InputContext *ic) {
  if (!ic)
    return;
  fcitx::Text empty;
  ic->inputPanel().setPreedit(empty);
  ic->inputPanel().setClientPreedit(empty);
  ic->updatePreedit();
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

fcitx::AddonInstance *
VinputEngineFactory::create(fcitx::AddonManager *manager) {
  return new VinputEngine(manager->instance());
}

FCITX_ADDON_FACTORY(VinputEngineFactory);
