#include "core/vinput.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/asr/model_manager.h"
#include "common/i18n.h"
#include "common/registry/registry_i18n.h"
#include "common/dbus/asr_backend_state_utils.h"
#include "common/runtime/runtime_defaults.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/string_utils.h"
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <systemd/sd-bus.h>
#include <thread>

namespace {

constexpr int kMenuPageSize = 10;

struct SceneOption {
  std::size_t index;
  std::string display_label;
  std::string search_text;
};

std::string SceneMenuTitle() { return _("Scenes /filter"); }

std::string AsrMenuTitle() { return _("Models /filter"); }

std::string CurrentItemText(const std::string &label) {
  if (label.empty()) {
    return {};
  }
  return std::string(_("Current: ")) + label;
}

std::string NormalizeSearchText(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

std::vector<std::string> SplitSearchTerms(const std::string &text) {
  std::vector<std::string> terms;
  std::istringstream stream(NormalizeSearchText(text));
  std::string term;
  while (stream >> term) {
    terms.push_back(std::move(term));
  }
  return terms;
}

bool MatchesAllTerms(const std::string &haystack, const std::string &query) {
  if (query.empty()) {
    return true;
  }

  const std::string normalized_haystack = NormalizeSearchText(haystack);
  for (const auto &term : SplitSearchTerms(query)) {
    if (normalized_haystack.find(term) == std::string::npos) {
      return false;
    }
  }
  return true;
}

void PopLastUtf8Char(std::string *text) {
  if (!text || text->empty()) {
    return;
  }

  std::size_t pos = text->size();
  do {
    --pos;
  } while (pos > 0 &&
           (static_cast<unsigned char>((*text)[pos]) & 0xC0) == 0x80);
  text->erase(pos);
}

bool MatchesSearch(const AsrMenuItem &item, const std::string &query) {
  return MatchesAllTerms(item.search_text, query);
}

bool MatchesSearch(const SceneOption &item, const std::string &query) {
  return MatchesAllTerms(item.search_text, query);
}

bool QueryAsrBackendStateFromUserBus(vinput::dbus::AsrBackendState *state) {
  sd_bus *bus = nullptr;
  if (sd_bus_open_user(&bus) < 0) {
    return false;
  }

  sd_bus_set_method_call_timeout(
      bus, static_cast<uint64_t>(vinput::runtime::kDbusCallTimeoutUsec));

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  const int r = sd_bus_call_method(bus, vinput::dbus::kBusName,
                                   vinput::dbus::kObjectPath,
                                   vinput::dbus::kInterface,
                                   vinput::dbus::kMethodGetAsrBackendState,
                                   &err, &reply, "");
  if (r < 0) {
    sd_bus_error_free(&err);
    if (reply) {
      sd_bus_message_unref(reply);
    }
    sd_bus_unref(bus);
    return false;
  }

  const char *target_provider = "";
  const char *target_model = "";
  const char *effective_provider = "";
  const char *effective_model = "";
  const char *last_error = "";
  int reload_in_progress = 0;
  int has_effective_backend = 0;
  if (sd_bus_message_read(reply, "sssssbb", &target_provider, &target_model,
                          &effective_provider, &effective_model, &last_error,
                          &reload_in_progress, &has_effective_backend) < 0) {
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    sd_bus_unref(bus);
    return false;
  }

  if (state) {
    state->target_provider_id = target_provider ? target_provider : "";
    state->target_model_id = target_model ? target_model : "";
    state->effective_provider_id = effective_provider ? effective_provider : "";
    state->effective_model_id = effective_model ? effective_model : "";
    state->last_error = last_error ? last_error : "";
    state->reload_in_progress = reload_in_progress != 0;
    state->has_effective_backend = has_effective_backend != 0;
  }

  sd_bus_message_unref(reply);
  sd_bus_error_free(&err);
  sd_bus_unref(bus);
  return true;
}

std::string ResultMenuTitle(std::size_t count) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), _("Choose Result (%zu)"), count);
  return buf;
}

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

std::string DecorateFilterTitle(const std::string &base_title,
                                const std::string &query,
                                bool filter_mode) {
  if (!filter_mode && query.empty()) {
    return base_title;
  }
  if (base_title.size() >= 2 &&
      base_title.compare(base_title.size() - 2, 2, " /") == 0) {
    return base_title + query;
  }
  return base_title + " / " + query;
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

void SetMenuTitle(fcitx::InputContext *ic, const std::string &base_title,
                  const std::string &query, bool filter_mode,
                  fcitx::CandidateList *candidate_list) {
  SetMenuTitle(ic, DecorateFilterTitle(base_title, query, filter_mode),
               candidate_list);
}

void SetMenuAuxDown(fcitx::InputContext *ic, const std::string &text) {
  if (!ic) {
    return;
  }

  fcitx::Text aux_down;
  aux_down.append(text);
  ic->inputPanel().setAuxDown(aux_down);
}

bool IsCtrlShortcut(const fcitx::Key &key, fcitx::KeySym sym) {
  auto matches = [sym](const fcitx::Key &candidate) {
    if (candidate.states() != fcitx::KeyState::Ctrl) {
      return false;
    }
    if (candidate.sym() == sym) {
      return true;
    }

    const uint32_t expected = fcitx::Key::keySymToUnicode(sym);
    const uint32_t actual = fcitx::Key::keySymToUnicode(candidate.sym());
    if (expected == 0 || actual == 0) {
      return false;
    }
    return std::tolower(static_cast<unsigned char>(actual)) ==
           std::tolower(static_cast<unsigned char>(expected));
  };

  return matches(key) || matches(key.normalize());
}

bool IsPureModifierKey(const fcitx::Key &key) {
  return key.normalize().isModifier();
}

bool HasNoModifiers(const fcitx::Key &key) {
  return key.normalize().states() == fcitx::KeyStates();
}

bool IsKeySym(const fcitx::Key &key, fcitx::KeySym sym) {
  const auto normalized = key.normalize();
  return normalized.sym() == sym && HasNoModifiers(normalized);
}

bool IsOneOfKeySyms(const fcitx::Key &key,
                    std::initializer_list<fcitx::KeySym> syms) {
  for (const auto sym : syms) {
    if (IsKeySym(key, sym)) {
      return true;
    }
  }
  return false;
}

bool IsPagePrevKey(const fcitx::Key &key) {
  return IsOneOfKeySyms(key, {FcitxKey_Page_Up, FcitxKey_KP_Page_Up});
}

bool IsPageNextKey(const fcitx::Key &key) {
  return IsOneOfKeySyms(key, {FcitxKey_Page_Down, FcitxKey_KP_Page_Down});
}

bool IsEnterKey(const fcitx::Key &key) {
  return IsOneOfKeySyms(key, {FcitxKey_Return, FcitxKey_KP_Enter});
}

bool IsEscapeKey(const fcitx::Key &key) { return IsKeySym(key, FcitxKey_Escape); }

bool IsSlashKey(const fcitx::Key &key) { return IsKeySym(key, FcitxKey_slash); }

bool IsBackspaceKey(const fcitx::Key &key) {
  return IsKeySym(key, FcitxKey_BackSpace);
}

bool IsUpKey(const fcitx::Key &key) { return IsKeySym(key, FcitxKey_Up); }

bool IsDownKey(const fcitx::Key &key) { return IsKeySym(key, FcitxKey_Down); }

bool IsPrintableMenuInput(const fcitx::Key &key, bool filter_mode) {
  if (!filter_mode) {
    return false;
  }

  if (IsEnterKey(key) || IsEscapeKey(key) || IsSlashKey(key) ||
      IsBackspaceKey(key) || IsUpKey(key) || IsDownKey(key) ||
      IsPagePrevKey(key) || IsPageNextKey(key) ||
      IsCtrlShortcut(key, FcitxKey_w) || IsCtrlShortcut(key, FcitxKey_u) ||
      IsPureModifierKey(key)) {
    return false;
  }

  const auto normalized = key.normalize();
  if (normalized.states() != fcitx::KeyStates()) {
    return false;
  }

  const std::string utf8 = fcitx::Key::keySymToUTF8(normalized.sym());
  if (utf8.empty()) {
    return false;
  }

  for (unsigned char ch : utf8) {
    if (ch < 0x20 || ch == 0x7f) {
      return false;
    }
  }
  return true;
}

void DeleteLastWord(std::string *text) {
  if (!text || text->empty()) {
    return;
  }

  while (!text->empty() &&
         static_cast<unsigned char>(text->back()) < 0x80 &&
         std::isspace(static_cast<unsigned char>(text->back()))) {
    text->pop_back();
  }
  while (!text->empty()) {
    const unsigned char ch = static_cast<unsigned char>(text->back());
    if (ch < 0x80 && std::isspace(ch)) {
      break;
    }
    PopLastUtf8Char(text);
  }
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

void SelectFirstCandidate(fcitx::CommonCandidateList *candidate_list) {
  if (!candidate_list || candidate_list->totalSize() <= 0) {
    return;
  }

  candidate_list->setGlobalCursorIndex(0);
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
  SceneCandidateWord(VinputEngine *engine, SceneOption option)
      : fcitx::CandidateWord(fcitx::Text(option.display_label)),
        engine_(engine), index_(option.index) {}

  void select(fcitx::InputContext *inputContext) const override {
    engine_->selectScene(index_, inputContext);
  }

private:
  VinputEngine *engine_;
  std::size_t index_;
};

class AsrCandidateWord : public fcitx::CandidateWord {
public:
  AsrCandidateWord(VinputEngine *engine, std::size_t index,
                   const std::string &label)
      : fcitx::CandidateWord(fcitx::Text(label)),
        engine_(engine), index_(index) {}

  void select(fcitx::InputContext *inputContext) const override {
    engine_->selectAsrItem(index_, inputContext);
  }

private:
  VinputEngine *engine_;
  std::size_t index_;
};

class ResultCandidateWord : public fcitx::CandidateWord {
public:
  ResultCandidateWord(VinputEngine *engine, std::size_t index,
                      const std::string &text, const std::string &comment)
      : fcitx::CandidateWord(fcitx::Text(text)),
        engine_(engine), index_(index) {
    if (!comment.empty()) {
#ifdef VINPUT_FCITX5_CORE_HAVE_SET_COMMENT
      setComment(fcitx::Text(comment));
#endif
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

void VinputEngine::showSceneMenu(fcitx::InputContext *ic) {
  if (!ic) {
    return;
  }

  reloadSceneConfig();
  scene_menu_ic_ = ic;
  scene_menu_visible_ = true;
  scene_menu_query_.clear();
  scene_menu_filter_mode_ = false;
  rebuildSceneMenu(ic);
}

void VinputEngine::rebuildSceneMenu(fcitx::InputContext *ic) {
  if (!ic) {
    return;
  }

  std::string active_label;
  auto candidate_list = std::make_unique<fcitx::CommonCandidateList>();
  candidate_list->setPageSize(kMenuPageSize);
  candidate_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
  candidate_list->setCursorPositionAfterPaging(
      fcitx::CursorPositionAfterPaging::ResetToFirst);

  scene_menu_filtered_indices_.clear();
  std::vector<SceneOption> scene_options;
  for (std::size_t i = 0; i < scene_config_.scenes.size(); ++i) {
    const auto &scene = scene_config_.scenes[i];
    const std::string label = vinput::scene::DisplayLabel(scene);
    const bool active = scene.id == active_scene_id_;
    if (active) {
      active_label = label;
    }
    scene_options.push_back(SceneOption{
        .index = i,
        .display_label = label,
        .search_text = label + " " + scene.id,
    });
    if (active) {
      continue;
    }
    if (MatchesSearch(scene_options.back(), scene_menu_query_)) {
      scene_menu_filtered_indices_.push_back(i);
    }
  }

  for (const std::size_t scene_index : scene_menu_filtered_indices_) {
    candidate_list->append<SceneCandidateWord>(this, scene_options[scene_index]);
  }
  SelectFirstCandidate(candidate_list.get());

  SetMenuTitle(ic, SceneMenuTitle(), scene_menu_query_, scene_menu_filter_mode_,
               candidate_list.get());
  SetMenuAuxDown(ic, CurrentItemText(active_label));
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
  scene_menu_query_.clear();
  scene_menu_filter_mode_ = false;
  scene_menu_filtered_indices_.clear();
  fcitx::Text empty;
  scene_menu_ic_->inputPanel().setAuxUp(empty);
  scene_menu_ic_->inputPanel().setAuxDown(empty);
  scene_menu_ic_->inputPanel().setCandidateList({});
  scene_menu_ic_->updateUserInterface(
      fcitx::UserInterfaceComponent::InputPanel);
  scene_menu_ic_ = nullptr;
}

bool VinputEngine::handleSceneMenuKeyEvent(fcitx::KeyEvent &keyEvent) {
  if (!scene_menu_visible_ || !scene_menu_ic_) {
    return false;
  }

  auto candidate_list = scene_menu_ic_->inputPanel().candidateList();
  auto *cursor_list =
      candidate_list ? candidate_list->toCursorMovable() : nullptr;
  const auto normalized_key = keyEvent.key().normalize();
  const bool printable_filter_input =
      IsPrintableMenuInput(keyEvent.key(), scene_menu_filter_mode_);
  const bool handled_key =
      keyEvent.key().checkKeyList(scene_menu_key_) ||
      IsPagePrevKey(keyEvent.key()) ||
      IsPageNextKey(keyEvent.key()) ||
      keyEvent.key().digitSelection() >= 0 ||
      IsSlashKey(keyEvent.key()) ||
      IsBackspaceKey(keyEvent.key()) ||
      IsCtrlShortcut(keyEvent.key(), FcitxKey_w) ||
      IsCtrlShortcut(keyEvent.key(), FcitxKey_u) ||
      IsUpKey(keyEvent.key()) ||
      IsDownKey(keyEvent.key()) ||
      IsEnterKey(keyEvent.key()) ||
      IsEscapeKey(keyEvent.key()) ||
      IsPureModifierKey(keyEvent.key()) ||
      printable_filter_input;

  if (keyEvent.isRelease()) {
    if (!handled_key) {
      return false;
    }
    keyEvent.filterAndAccept();
    return true;
  }

  if (!handled_key) {
    hideSceneMenu();
    return false;
  }

  if (keyEvent.key().checkKeyList(scene_menu_key_) ||
      IsPureModifierKey(keyEvent.key())) {
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsEscapeKey(keyEvent.key())) {
    if (scene_menu_filter_mode_ || !scene_menu_query_.empty()) {
      scene_menu_query_.clear();
      scene_menu_filter_mode_ = false;
      rebuildSceneMenu(scene_menu_ic_);
    } else {
      hideSceneMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsSlashKey(keyEvent.key())) {
    scene_menu_filter_mode_ = true;
    rebuildSceneMenu(scene_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsBackspaceKey(keyEvent.key()) && scene_menu_filter_mode_) {
    if (!scene_menu_query_.empty()) {
      PopLastUtf8Char(&scene_menu_query_);
    } else {
      scene_menu_filter_mode_ = false;
    }
    rebuildSceneMenu(scene_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (scene_menu_filter_mode_ && IsCtrlShortcut(keyEvent.key(), FcitxKey_w)) {
    DeleteLastWord(&scene_menu_query_);
    if (scene_menu_query_.empty()) {
      scene_menu_filter_mode_ = false;
    }
    rebuildSceneMenu(scene_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (scene_menu_filter_mode_ && IsCtrlShortcut(keyEvent.key(), FcitxKey_u)) {
    scene_menu_query_.clear();
    scene_menu_filter_mode_ = false;
    rebuildSceneMenu(scene_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (printable_filter_input) {
    const std::string utf8 = fcitx::Key::keySymToUTF8(normalized_key.sym());
    scene_menu_query_.append(utf8);
    rebuildSceneMenu(scene_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsPagePrevKey(keyEvent.key())) {
    ChangeCandidatePage(
        scene_menu_ic_,
        DecorateFilterTitle(SceneMenuTitle(), scene_menu_query_,
                            scene_menu_filter_mode_),
        false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsPageNextKey(keyEvent.key())) {
    ChangeCandidatePage(
        scene_menu_ic_,
        DecorateFilterTitle(SceneMenuTitle(), scene_menu_query_,
                            scene_menu_filter_mode_),
        true);
    keyEvent.filterAndAccept();
    return true;
  }

  const int digit = keyEvent.key().digitSelection();
  const int digit_index = DigitSelectionIndex(candidate_list.get(), digit);
  if (digit >= 0 &&
      digit_index < static_cast<int>(scene_menu_filtered_indices_.size())) {
    selectScene(scene_menu_filtered_indices_[digit_index], scene_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && IsUpKey(keyEvent.key())) {
    cursor_list->prevCandidate();
    scene_menu_ic_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && IsDownKey(keyEvent.key())) {
    cursor_list->nextCandidate();
    scene_menu_ic_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsEnterKey(keyEvent.key())) {
    int index = CurrentSelectionIndex(candidate_list.get());
    if (index < 0) {
      index = scene_menu_filtered_indices_.empty() ? -1 : 0;
    }
    if (index >= 0 &&
        index < static_cast<int>(scene_menu_filtered_indices_.size())) {
      selectScene(scene_menu_filtered_indices_[index], scene_menu_ic_);
    } else {
      hideSceneMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  hideSceneMenu();
  return false;
}

void VinputEngine::selectScene(std::size_t index, fcitx::InputContext *ic) {
  if (index >= scene_config_.scenes.size()) {
    hideSceneMenu();
    return;
  }

  const std::string selected_scene_id = scene_config_.scenes[index].id;
  // Persist the active scene to config
  auto core_config = LoadCoreConfig();
  core_config.scenes.activeScene = selected_scene_id;
  if (!SaveCoreConfig(core_config)) {
    notifyError(_("Failed to save active scene."));
    return;
  }
  active_scene_id_ = selected_scene_id;
  scene_config_.activeSceneId = selected_scene_id;
  hideSceneMenu();
  notifyInfo(vinput::str::FmtStr(_("Switched scene to '%s'."),
                                 vinput::scene::DisplayLabel(
                                     scene_config_.scenes[index]).c_str()));
  (void)ic;
}

void VinputEngine::reloadAsrMenuItems() {
  asr_menu_items_.clear();
  auto core_config = LoadCoreConfig();
  const std::string configured_provider = core_config.asr.activeProvider;
  const std::string configured_model = ResolvePreferredLocalModel(core_config);
  vinput::dbus::AsrBackendState backend_state;
  const bool have_backend_state = has_cached_asr_backend_state_;
  if (have_backend_state) {
    backend_state = cached_asr_backend_state_;
  }
  const std::string active_provider =
      have_backend_state && !backend_state.effective_provider_id.empty()
          ? backend_state.effective_provider_id
          : configured_provider;
  const std::string active_model =
      have_backend_state && !backend_state.effective_model_id.empty()
          ? backend_state.effective_model_id
          : configured_model;
  const auto i18n_map = vinput::registry::FetchMergedI18nMap(
      core_config, vinput::registry::DetectPreferredLocale(), nullptr);

  for (const auto &provider : core_config.asr.providers) {
    const std::string &pid = AsrProviderId(provider);
    const std::string provider_title =
        vinput::registry::LookupI18n(i18n_map, pid + ".title", pid);
    if (std::holds_alternative<LocalAsrProvider>(provider)) {
      // Enumerate installed models under this local provider
      const auto base_dir = ResolveModelBaseDir(core_config);
      ModelManager manager(base_dir.string());
      auto models = manager.ListDetailed(active_model);
      for (const auto &summary : models) {
        const bool item_active =
            (pid == active_provider) && (summary.id == active_model);
        std::string model_title = summary.title;
        if (model_title.empty()) {
          model_title = vinput::registry::LookupI18n(i18n_map,
                                                     summary.id + ".title",
                                                     summary.id);
        }
        std::string label = model_title + " [local]";
        if (have_backend_state && backend_state.reload_in_progress &&
            pid == backend_state.target_provider_id &&
            summary.id == backend_state.target_model_id &&
            ((pid != active_provider) || (summary.id != active_model))) {
          label += " (loading)";
        }
        asr_menu_items_.push_back(AsrMenuItem{
            .provider_id = pid,
            .model_id = summary.id,
            .display_label = label,
            .search_text = label + " " + summary.id + " " + model_title + " " +
                           summary.language + " " + pid + " " + provider_title,
            .active = item_active,
        });
      }
    } else {
      // Command provider — one row
      const bool item_active = (pid == active_provider);
      std::string label = provider_title + " [command]";
      if (have_backend_state && backend_state.reload_in_progress &&
          pid == backend_state.target_provider_id &&
          ((pid != active_provider) || !backend_state.target_model_id.empty())) {
        label += " (loading)";
      }
      asr_menu_items_.push_back(AsrMenuItem{
          .provider_id = pid,
          .model_id = {},
          .display_label = label,
          .search_text = label + " " + pid + " " + provider_title,
          .active = item_active,
      });
    }
  }
}

void VinputEngine::requestAsrMenuStateRefresh(fcitx::InputContext *ic) {
  if (!ic || !lifetime_token_) {
    return;
  }

  const auto seq = ++asr_state_refresh_seq_;
  auto ic_ref = ic->watch();
  std::weak_ptr<bool> lifetime_weak = lifetime_token_;

  std::thread([this, seq, ic_ref, lifetime_weak]() mutable {
    if (lifetime_weak.expired()) {
      return;
    }

    vinput::dbus::AsrBackendState state;
    const bool ok = QueryAsrBackendStateFromUserBus(&state);

    if (lifetime_weak.expired()) {
      return;
    }

    event_dispatcher_.schedule(
        [this, seq, ic_ref, state = std::move(state), ok, lifetime_weak]() {
          if (!ic_ref.isValid()) {
            return;
          }
          if (lifetime_weak.expired() || seq != asr_state_refresh_seq_) {
            return;
          }
          if (!ok) {
            return;
          }
          cached_asr_backend_state_ = state;
          has_cached_asr_backend_state_ = true;
          if (asr_menu_visible_ && asr_menu_ic_) {
            reloadAsrMenuItems();
            rebuildAsrMenu(asr_menu_ic_);
          }
        });
  }).detach();
}

void VinputEngine::rebuildAsrMenu(fcitx::InputContext *ic) {
  if (!ic) {
    return;
  }

  std::string active_label;
  asr_menu_filtered_indices_.clear();
  for (std::size_t i = 0; i < asr_menu_items_.size(); ++i) {
    const auto &item = asr_menu_items_[i];
    if (item.active) {
      active_label = item.display_label;
      continue;
    }
    if (MatchesSearch(item, asr_menu_query_)) {
      asr_menu_filtered_indices_.push_back(i);
    }
  }

  auto candidate_list = std::make_unique<fcitx::CommonCandidateList>();
  candidate_list->setPageSize(kMenuPageSize);
  candidate_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
  candidate_list->setCursorPositionAfterPaging(
      fcitx::CursorPositionAfterPaging::ResetToFirst);

  for (const std::size_t item_index : asr_menu_filtered_indices_) {
    const auto &item = asr_menu_items_[item_index];
    candidate_list->append<AsrCandidateWord>(
        this, item_index, item.display_label);
  }
  SelectFirstCandidate(candidate_list.get());

  SetMenuTitle(ic, AsrMenuTitle(), asr_menu_query_, asr_menu_filter_mode_,
               candidate_list.get());
  SetMenuAuxDown(ic, CurrentItemText(active_label));
  ic->inputPanel().setCandidateList(std::move(candidate_list));
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VinputEngine::showAsrMenu(fcitx::InputContext *ic) {
  if (!ic) {
    return;
  }

  reloadAsrMenuItems();
  asr_menu_ic_ = ic;
  asr_menu_visible_ = true;
  asr_menu_query_.clear();
  asr_menu_filter_mode_ = false;
  rebuildAsrMenu(ic);
  requestAsrMenuStateRefresh(ic);
}

void VinputEngine::hideAsrMenu() {
  if (!asr_menu_visible_ || !asr_menu_ic_) {
    asr_menu_visible_ = false;
    asr_menu_ic_ = nullptr;
    return;
  }

  asr_menu_visible_ = false;
  asr_menu_query_.clear();
  asr_menu_filter_mode_ = false;
  asr_menu_filtered_indices_.clear();
  fcitx::Text empty;
  asr_menu_ic_->inputPanel().setAuxUp(empty);
  asr_menu_ic_->inputPanel().setAuxDown(empty);
  asr_menu_ic_->inputPanel().setCandidateList({});
  asr_menu_ic_->updateUserInterface(
      fcitx::UserInterfaceComponent::InputPanel);
  asr_menu_ic_ = nullptr;
}

bool VinputEngine::handleAsrMenuKeyEvent(fcitx::KeyEvent &keyEvent) {
  if (!asr_menu_visible_ || !asr_menu_ic_) {
    return false;
  }

  auto candidate_list = asr_menu_ic_->inputPanel().candidateList();
  auto *cursor_list =
      candidate_list ? candidate_list->toCursorMovable() : nullptr;
  const auto normalized_key = keyEvent.key().normalize();

  const bool printable_filter_input =
      IsPrintableMenuInput(keyEvent.key(), asr_menu_filter_mode_);
  const bool handled_key =
      keyEvent.key().checkKeyList(asr_menu_key_) ||
      IsPagePrevKey(keyEvent.key()) ||
      IsPageNextKey(keyEvent.key()) ||
      keyEvent.key().digitSelection() >= 0 ||
      IsSlashKey(keyEvent.key()) ||
      IsBackspaceKey(keyEvent.key()) ||
      IsCtrlShortcut(keyEvent.key(), FcitxKey_w) ||
      IsCtrlShortcut(keyEvent.key(), FcitxKey_u) ||
      IsUpKey(keyEvent.key()) ||
      IsDownKey(keyEvent.key()) ||
      IsEnterKey(keyEvent.key()) ||
      IsEscapeKey(keyEvent.key()) ||
      IsPureModifierKey(keyEvent.key()) ||
      printable_filter_input;

  if (keyEvent.isRelease()) {
    if (!handled_key) {
      return false;
    }
    keyEvent.filterAndAccept();
    return true;
  }

  if (!handled_key) {
    hideAsrMenu();
    return false;
  }

  if (keyEvent.key().checkKeyList(asr_menu_key_) ||
      IsPureModifierKey(keyEvent.key())) {
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsEscapeKey(keyEvent.key())) {
    if (asr_menu_filter_mode_ || !asr_menu_query_.empty()) {
      asr_menu_query_.clear();
      asr_menu_filter_mode_ = false;
      rebuildAsrMenu(asr_menu_ic_);
    } else {
      hideAsrMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsSlashKey(keyEvent.key())) {
    asr_menu_filter_mode_ = true;
    rebuildAsrMenu(asr_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsBackspaceKey(keyEvent.key()) && asr_menu_filter_mode_) {
    if (!asr_menu_query_.empty()) {
      PopLastUtf8Char(&asr_menu_query_);
    } else {
      asr_menu_filter_mode_ = false;
    }
    rebuildAsrMenu(asr_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (asr_menu_filter_mode_ && IsCtrlShortcut(keyEvent.key(), FcitxKey_w)) {
    DeleteLastWord(&asr_menu_query_);
    if (asr_menu_query_.empty()) {
      asr_menu_filter_mode_ = false;
    }
    rebuildAsrMenu(asr_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (asr_menu_filter_mode_ && IsCtrlShortcut(keyEvent.key(), FcitxKey_u)) {
    asr_menu_query_.clear();
    asr_menu_filter_mode_ = false;
    rebuildAsrMenu(asr_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (printable_filter_input) {
    const std::string utf8 = fcitx::Key::keySymToUTF8(normalized_key.sym());
    asr_menu_query_.append(utf8);
    rebuildAsrMenu(asr_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsPagePrevKey(keyEvent.key())) {
    ChangeCandidatePage(
        asr_menu_ic_,
        DecorateFilterTitle(AsrMenuTitle(), asr_menu_query_,
                            asr_menu_filter_mode_),
        false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsPageNextKey(keyEvent.key())) {
    ChangeCandidatePage(
        asr_menu_ic_,
        DecorateFilterTitle(AsrMenuTitle(), asr_menu_query_,
                            asr_menu_filter_mode_),
        true);
    keyEvent.filterAndAccept();
    return true;
  }

  const int digit = keyEvent.key().digitSelection();
  const int digit_index = DigitSelectionIndex(candidate_list.get(), digit);
  if (digit >= 0 &&
      digit_index < static_cast<int>(asr_menu_filtered_indices_.size())) {
    selectAsrItem(asr_menu_filtered_indices_[digit_index], asr_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && IsUpKey(keyEvent.key())) {
    cursor_list->prevCandidate();
    asr_menu_ic_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && IsDownKey(keyEvent.key())) {
    cursor_list->nextCandidate();
    asr_menu_ic_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsEnterKey(keyEvent.key())) {
    int index = CurrentSelectionIndex(candidate_list.get());
    if (index < 0) {
      index = asr_menu_filtered_indices_.empty() ? -1 : 0;
    }
    if (index >= 0 &&
        index < static_cast<int>(asr_menu_filtered_indices_.size())) {
      selectAsrItem(asr_menu_filtered_indices_[index], asr_menu_ic_);
    } else {
      hideAsrMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  hideAsrMenu();
  return false;
}

void VinputEngine::selectAsrItem(std::size_t index, fcitx::InputContext *ic) {
  if (index >= asr_menu_items_.size()) {
    hideAsrMenu();
    return;
  }

  const AsrMenuItem &item = asr_menu_items_[index];
  auto core_config = LoadCoreConfig();
  core_config.asr.activeProvider = item.provider_id;
  if (!item.model_id.empty()) {
    std::string error;
    if (!SetPreferredLocalModel(&core_config, item.model_id, &error)) {
      notifyError(error);
      hideAsrMenu();
      return;
    }
  }
  if (!SaveCoreConfig(core_config)) {
    notifyError(_("Failed to save ASR config."));
    hideAsrMenu();
    return;
  }
  if (has_cached_asr_backend_state_) {
    cached_asr_backend_state_.target_provider_id = item.provider_id;
    cached_asr_backend_state_.target_model_id = item.model_id;
    cached_asr_backend_state_.reload_in_progress = true;
    cached_asr_backend_state_.last_error.clear();
  }
  if (!queryDaemonStatus().empty()) {
    std::string reload_error;
    if (!callReloadAsrBackend(&reload_error)) {
      notifyError(reload_error.empty() ? _("Failed to reload ASR backend.")
                                       : reload_error);
      hideAsrMenu();
      return;
    }
  }
  hideAsrMenu();
  notifyInfo(vinput::str::FmtStr(_("ASR switch requested for '%s'."),
                                 item.display_label.c_str()));
  requestAsrMenuStateRefresh(ic);
  (void)ic;
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

void VinputEngine::selectResultCandidate(std::size_t index,
                                         fcitx::InputContext *ic) {
  if (index >= result_candidates_.size()) {
    hideResultMenu();
    result_is_command_ = false;
    return;
  }

  const auto candidate = result_candidates_[index];
  const std::string text = candidate.text;
  const bool is_command_result = result_is_command_;
  hideResultMenu();
  result_is_command_ = false;
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
    if (candidate.source == vinput::result::kSourceLlm) {
      appendContextEntry(text, "llm");
    }
    suppressNextCommitContext(text);
    clearPreedit(ic);
    ic->commitString(text);
  }
}
