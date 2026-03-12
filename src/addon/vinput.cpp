#include "vinput.h"
#include "common/core_config.h"
#include "common/dbus_interface.h"
#include "common/i18n.h"
#include "common/postprocess_scene.h"

#include "clipboard_public.h"
#include "notifications_public.h"
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
#include <cstdlib>
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

std::string SceneMenuTitle() { return _("Choose Postprocess Menu"); }

std::string ResultMenuTitle(std::size_t count) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), _("Choose Result (%zu)"), count);
  return buf;
}

std::string RecordingPreeditText() { return _("... Recording ..."); }

std::string CommandingPreeditText() { return _("... Commanding ..."); }

std::string InferringPreeditText() { return _("... Recognizing ..."); }

std::string PostprocessingPreeditText() { return _("... Postprocessing ..."); }

std::string NoSelectionPreeditText() { return _("Please select text first."); }

std::string LlmNotEnabledPreeditText() { return _("Please enable LLM first."); }

std::string ResultCandidateComment(const vinput::result::Candidate &candidate,
                                    std::size_t llm_index) {
  if (candidate.source == vinput::result::kSourceRaw) {
    return _("Original");
  }
  if (candidate.source == vinput::result::kSourceAsr) {
    return _("Voice Command");
  }
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%zu", llm_index);
  return buf;
}

std::string DisplayCandidateText(std::string text) {
  for (char &ch : text) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      ch = ' ';
    }
  }
  return text;
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

  char buf[64];
  std::snprintf(buf, sizeof(buf), _(" (%d/%d)"), current_page + 1, total_pages);
  return base_title + buf;
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
            option.label, active ? _(" (Current)") : std::string()))),
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
      : fcitx::CandidateWord(fcitx::Text(DisplayCandidateText(text))),
        engine_(engine), index_(index) {
    if (!comment.empty()) {
      setComment(fcitx::Text(comment));
    }
  }

  void select(fcitx::InputContext *inputContext) const override {
    engine_->selectResultCandidate(index_, inputContext);
  }

private:
  VinputEngine *engine_;
  std::size_t index_;
};

} // namespace

VinputEngine::VinputEngine(fcitx::Instance *instance) : instance_(instance) {
  vinput::i18n::Init();
  reloadConfig();

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextKeyEvent,
      fcitx::EventWatcherPhase::PreInputMethod,
      [this](fcitx::Event &event) { handleKeyEvent(event); }));

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextCreated,
      fcitx::EventWatcherPhase::PreInputMethod,
      [](fcitx::Event &event) {
        auto &icEvent = static_cast<fcitx::InputContextEvent &>(event);
        auto *ic = icEvent.inputContext();
        ic->setCapabilityFlags(ic->capabilityFlags() |
                               fcitx::CapabilityFlag::SurroundingText);
      }));

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

  fcitx::dbus::MatchRule llm_error_rule(kBusName, kObjectPath, kInterface,
                                        kSignalLlmError);

  llm_error_slot_ =
      bus_->addMatch(llm_error_rule, [this](fcitx::dbus::Message &msg) {
        onLlmError(msg);
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

  if (!recording_ && keyEvent.key().checkKeyList(scene_menu_key_) &&
      !keyEvent.isRelease()) {
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

  FCITX_LOG(Info) << "vinput handleKeyEvent: " << keyEvent.key()
                   << " isRelease=" << keyEvent.isRelease()
                   << " is_trigger=" << is_trigger
                   << " is_command=" << is_command;

  if ((is_trigger || is_command) && !keyEvent.isRelease()) {
    cancelPendingStop();
    if (!recording_) {
      recording_ = true;
      active_trigger_ = is_trigger ? trigger_keys_[trigger_index]
                                   : command_keys_[command_index];
      active_ic_ = keyEvent.inputContext();
      hideResultMenu();

      if (is_command) {
        // Check LLM is enabled before proceeding
        {
          auto core_config = LoadCoreConfig();
          if (!core_config.llm.enabled ||
              ResolveActiveLlmProvider(core_config) == nullptr) {
            recording_ = false;
            updatePreedit(active_ic_, LlmNotEnabledPreeditText());
            keyEvent.filterAndAccept();
            return;
          }
        }
        command_mode_ = true;
        std::string selected_text;
        auto &surrounding = active_ic_->surroundingText();
        if (surrounding.isValid() && surrounding.cursor() != surrounding.anchor()) {
          const auto &text = surrounding.text();
          int from = std::min(surrounding.cursor(), surrounding.anchor());
          int to = std::max(surrounding.cursor(), surrounding.anchor());
          selected_text = text.substr(from, to - from);
        }
        if (selected_text.empty()) {
          if (auto *clipboard = instance_->addonManager().addon("clipboard")) {
            selected_text =
                clipboard->call<fcitx::IClipboard::primary>(active_ic_);
          }
        }
        if (selected_text.empty()) {
          command_mode_ = false;
          recording_ = false;
          updatePreedit(active_ic_, NoSelectionPreeditText());
          keyEvent.filterAndAccept();
          return;
        }
        FCITX_LOG(Info) << "vinput: command key pressed, selected_text length=" << selected_text.size();
        callStartCommandRecording(selected_text);
        updatePreedit(active_ic_, CommandingPreeditText());
      } else {
        FCITX_LOG(Info) << "vinput: trigger key pressed";
        callStartRecording();
        updatePreedit(active_ic_, RecordingPreeditText());
      }
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

  if ((is_trigger || is_command) && keyEvent.isRelease()) {
    if (!recording_ && active_ic_) {
      clearPreedit(active_ic_);
      active_ic_ = nullptr;
    }
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
  command_keys_ = settings_.commandKeys;
  scene_menu_key_ = settings_.sceneMenuKeys;
  page_prev_keys_ = settings_.pagePrevKeys;
  page_next_keys_ = settings_.pageNextKeys;
}

void VinputEngine::reloadSceneConfig() {
  auto core_config = LoadCoreConfig();
  scene_config_.activeSceneId = core_config.scenes.activeScene;
  scene_config_.scenes = core_config.scenes.definitions;
  if (!scene_config_.scenes.empty()) {
    active_scene_id_ =
        vinput::scene::Resolve(scene_config_, active_scene_id_).id;
  }
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
            .label = vinput::scene::DisplayLabel(scene),
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
    if (keyEvent.key().checkKeyList(scene_menu_key_) ||
        keyEvent.key().checkKeyList(page_prev_keys_) ||
        keyEvent.key().checkKeyList(page_next_keys_) ||
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

  if (keyEvent.key().checkKeyList(scene_menu_key_)) {
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().check(FcitxKey_Escape)) {
    hideSceneMenu();
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().checkKeyList(page_prev_keys_)) {
    ChangeCandidatePage(scene_menu_ic_, SceneMenuTitle(), false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().checkKeyList(page_next_keys_)) {
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
        keyEvent.key().checkKeyList(page_prev_keys_) ||
        keyEvent.key().checkKeyList(page_next_keys_) ||
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

  if (keyEvent.key().checkKeyList(page_prev_keys_)) {
    ChangeCandidatePage(result_menu_ic_,
                        ResultMenuTitle(result_candidates_.size()), false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().checkKeyList(page_next_keys_)) {
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
  // Persist the active scene to config
  auto core_config = LoadCoreConfig();
  core_config.scenes.activeScene = active_scene_id_;
  SaveCoreConfig(core_config);
  hideSceneMenu();
  (void)ic;
}

void VinputEngine::selectResultCandidate(std::size_t index,
                                         fcitx::InputContext *ic) {
  if (index >= result_candidates_.size()) {
    hideResultMenu();
    command_mode_ = false;
    return;
  }

  const auto &candidate = result_candidates_[index];
  const std::string text = candidate.text;
  const bool is_command_result = command_mode_;
  hideResultMenu();
  command_mode_ = false;
  if (!ic) {
    return;
  }

  if (candidate.source == vinput::result::kSourceCancel) {
    clearPreedit(ic);
    return;
  }

  if (!text.empty()) {
    // command 模式：先用 surrounding text 删除选中内容
    if (is_command_result) {
      auto &surrounding = ic->surroundingText();
      if (surrounding.isValid() && surrounding.cursor() != surrounding.anchor()) {
        int cursor = surrounding.cursor();
        int anchor = surrounding.anchor();
        int from = std::min(cursor, anchor);
        int len = std::abs(cursor - anchor);
        ic->deleteSurroundingText(from - cursor, len);
      }
    }
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

void VinputEngine::callStartCommandRecording(const std::string &selected_text) {
  if (!bus_)
    return;
  auto msg = bus_->createMethodCall(kBusName, kObjectPath, kInterface,
                                    kMethodStartCommandRecording);
  msg << selected_text;
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
    command_mode_ = false;
    return;
  }

  hideResultMenu();

  const auto payload = vinput::result::Parse(payload_text);
  clearPreedit(active_ic_);

  // command 模式：先用 surrounding text 删除选中内容
  if (command_mode_) {
    auto &surrounding = active_ic_->surroundingText();
    if (surrounding.isValid() && surrounding.cursor() != surrounding.anchor()) {
      int cursor = surrounding.cursor();
      int anchor = surrounding.anchor();
      int from = std::min(cursor, anchor);
      int len = std::abs(cursor - anchor);
      active_ic_->deleteSurroundingText(from - cursor, len);
    }
    command_mode_ = false;
  }

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
    updatePreedit(active_ic_, command_mode_ ? CommandingPreeditText() : RecordingPreeditText());
  } else if (status == "inferring") {
    updatePreedit(active_ic_, InferringPreeditText());
  } else if (status == "postprocessing") {
    updatePreedit(active_ic_, PostprocessingPreeditText());
  } else {
    clearPreedit(active_ic_);
  }
}

void VinputEngine::onLlmError(fcitx::dbus::Message &msg) {
  std::string error_message;
  msg >> error_message;

  if (error_message.empty()) {
    return;
  }

  auto *notifications =
      instance_->addonManager().addon("notifications", true);
  if (notifications) {
    notifications->call<fcitx::INotifications::sendNotification>(
        "fcitx5-vinput", 0, "dialog-error",
        _("Voice Input"), error_message, std::vector<std::string>{},
        5000, fcitx::NotificationActionCallback{},
        fcitx::NotificationClosedCallback{});
  } else {
    fprintf(stderr, "vinput: LLM error: %s\n", error_message.c_str());
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

#ifdef VINPUT_FCITX5_CORE_HAVE_ADDON_FACTORY_V2
FCITX_ADDON_FACTORY_V2(vinput, VinputEngineFactory);
#else
FCITX_ADDON_FACTORY(VinputEngineFactory);
#endif
