#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fcitx-utils/key.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/asr/model_manager.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/llm/adapter_manager.h"
#include "common/llm/provider_model_cache.h"
#include "common/registry/registry_i18n.h"
#include "common/runtime/runtime_defaults.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/string_utils.h"

#include "config.h"
#include "core/vinput.h"

namespace {

constexpr int kMenuPageSize = 10;

struct ParsedPaletteQuery {
  std::optional<PaletteCategory> scope;
  std::string terms;
};

std::string NormalizeSearchText(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

std::vector<std::string> SplitSearchTerms(const std::string& text) {
  std::vector<std::string> terms;
  std::istringstream stream(NormalizeSearchText(text));
  std::string term;
  while (stream >> term) {
    terms.push_back(std::move(term));
  }
  return terms;
}

bool MatchesAllTerms(const std::string& haystack, const std::string& query) {
  if (query.empty()) {
    return true;
  }

  const std::string normalized_haystack = NormalizeSearchText(haystack);
  for (const auto& term : SplitSearchTerms(query)) {
    if (normalized_haystack.find(term) == std::string::npos) {
      return false;
    }
  }
  return true;
}

void PopLastUtf8Char(std::string* text) {
  if (!text || text->empty()) {
    return;
  }

  std::size_t pos = text->size();
  do {
    --pos;
  } while (pos > 0 && (static_cast<unsigned char>((*text)[pos]) & 0xC0) == 0x80);
  text->erase(pos);
}

void DeleteLastWord(std::string* text) {
  if (!text || text->empty()) {
    return;
  }

  while (!text->empty() && static_cast<unsigned char>(text->back()) < 0x80 &&
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

ParsedPaletteQuery ParsePaletteQuery(std::string_view raw_query) {
  ParsedPaletteQuery res;
  std::string_view q = raw_query;
  while (!q.empty() && std::isspace(static_cast<unsigned char>(q.front()))) {
    q.remove_prefix(1);
  }

  if (q.size() >= 2 && q[0] == '/') {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(q[1])));
    if (c == 'a') {
      res.scope = PaletteCategory::Asr;
      q.remove_prefix(2);
    } else if (c == 's') {
      res.scope = PaletteCategory::Scene;
      q.remove_prefix(2);
    } else if (c == 'm') {
      res.scope = PaletteCategory::CommandModel;
      q.remove_prefix(2);
    } else if (c == 'p') {
      res.scope = PaletteCategory::Adapter;
      q.remove_prefix(2);
    }
  }

  while (!q.empty() && (std::isspace(static_cast<unsigned char>(q.front())) || q.front() == '/')) {
    q.remove_prefix(1);
  }
  res.terms = std::string(q);
  return res;
}

std::string PaletteMenuTitle(const ParsedPaletteQuery& parsed, bool filter_mode) {
  std::string base;
  if (parsed.scope.has_value()) {
    switch (*parsed.scope) {
    case PaletteCategory::Asr:
      base = _("ASR /a");
      break;
    case PaletteCategory::Scene:
      base = _("Scenes /s");
      break;
    case PaletteCategory::CommandModel:
      base = _("Command Models /m");
      break;
    case PaletteCategory::Adapter:
      base = _("Adapters /p");
      break;
    }
  } else {
    base = _("Palette (/a /s /m /p)");
  }
  if (filter_mode || !parsed.terms.empty()) {
    return base + ": " + parsed.terms;
  }
  return base;
}

std::string ResultMenuTitle(std::size_t count) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), _("Choose Result (%zu)"), count);
  return buf;
}

std::string ResultCandidateComment(const vinput::result::Candidate& candidate) {
  if (candidate.source == vinput::result::kSourceRaw) {
    return _("Original");
  }
  if (candidate.source == vinput::result::kSourceAsr) {
    return _("Voice Command");
  }
  return {};
}

std::string DecoratePagedMenuTitle(const std::string& base_title,
                                   fcitx::CandidateList* candidate_list) {
  auto* pageable = candidate_list ? candidate_list->toPageable() : nullptr;
  if (!pageable) {
    return base_title;
  }

  const int total_pages = pageable->totalPages();
  const int current_page = pageable->currentPage() + 1;
  if (total_pages <= 1) {
    return base_title;
  }

  char buf[64];
  std::snprintf(buf, sizeof(buf), " (%d/%d)", current_page, total_pages);
  return base_title + buf;
}

void SetMenuTitle(fcitx::InputContext* ic, const std::string& base_title,
                  fcitx::CandidateList* candidate_list) {
  if (!ic) {
    return;
  }

  fcitx::Text title(DecoratePagedMenuTitle(base_title, candidate_list));
  ic->inputPanel().setAuxUp(title);
}

bool IsCtrlShortcut(const fcitx::Key& key, fcitx::KeySym sym) {
  auto matches = [sym](const fcitx::Key& candidate) {
    return candidate.sym() == sym && candidate.hasCtrl() && !candidate.hasAlt() &&
           !candidate.hasSuper();
  };

  return matches(key) || matches(key.normalize());
}

bool IsPureModifierKey(const fcitx::Key& key) {
  return key.normalize().isModifier();
}

bool HasNoModifiers(const fcitx::Key& key) {
  return key.normalize().states() == fcitx::KeyStates();
}

bool IsKeySym(const fcitx::Key& key, fcitx::KeySym sym) {
  const auto normalized = key.normalize();
  return normalized.sym() == sym && HasNoModifiers(normalized);
}

bool IsOneOfKeySyms(const fcitx::Key& key, std::initializer_list<fcitx::KeySym> syms) {
  for (const auto sym : syms) {
    if (IsKeySym(key, sym)) {
      return true;
    }
  }
  return false;
}

bool IsPagePrevKey(const fcitx::Key& key) {
  return IsOneOfKeySyms(key, {FcitxKey_Page_Up, FcitxKey_KP_Page_Up});
}

bool IsPageNextKey(const fcitx::Key& key) {
  return IsOneOfKeySyms(key, {FcitxKey_Page_Down, FcitxKey_KP_Page_Down});
}

bool IsEnterKey(const fcitx::Key& key) {
  return IsOneOfKeySyms(key, {FcitxKey_Return, FcitxKey_KP_Enter});
}

bool IsEscapeKey(const fcitx::Key& key) {
  return IsKeySym(key, FcitxKey_Escape);
}

bool IsSlashKey(const fcitx::Key& key) {
  return IsKeySym(key, FcitxKey_slash);
}

bool IsBackspaceKey(const fcitx::Key& key) {
  return IsKeySym(key, FcitxKey_BackSpace);
}

bool IsUpKey(const fcitx::Key& key) {
  return IsKeySym(key, FcitxKey_Up);
}

bool IsDownKey(const fcitx::Key& key) {
  return IsKeySym(key, FcitxKey_Down);
}

bool IsPrintableMenuInput(const fcitx::Key& key, bool filter_mode) {
  if (!filter_mode) {
    return false;
  }

  if (IsEnterKey(key) || IsEscapeKey(key) || IsSlashKey(key) || IsBackspaceKey(key) ||
      IsUpKey(key) || IsDownKey(key) || IsPagePrevKey(key) || IsPageNextKey(key) ||
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

int DigitSelectionIndex(fcitx::CandidateList* candidate_list, int digit) {
  auto* pageable = candidate_list ? candidate_list->toPageable() : nullptr;
  int current_page = pageable ? pageable->currentPage() : 0;
  if (current_page < 0) {
    current_page = 0;
  }
  return current_page * kMenuPageSize + digit;
}

int CurrentSelectionIndex(fcitx::CandidateList* candidate_list) {
  if (!candidate_list) {
    return -1;
  }

  int current_index = candidate_list->cursorIndex();
  if (current_index < 0) {
    return -1;
  }

  auto* pageable = candidate_list->toPageable();
  int current_page = pageable ? pageable->currentPage() : 0;
  if (current_page < 0) {
    current_page = 0;
  }

  return current_page * kMenuPageSize + current_index;
}

void MoveCursorToIndex(fcitx::CandidateList* candidate_list, int target_index) {
  auto* cursor_list = candidate_list ? candidate_list->toCursorMovable() : nullptr;
  if (!cursor_list || target_index <= 0) {
    return;
  }

  for (int i = 0; i < target_index; ++i) {
    cursor_list->nextCandidate();
  }
}

void SelectFirstCandidate(fcitx::CommonCandidateList* candidate_list) {
  if (!candidate_list || candidate_list->totalSize() <= 0) {
    return;
  }

  candidate_list->setGlobalCursorIndex(0);
}

bool ChangeCandidatePage(fcitx::InputContext* ic, const std::string& base_title, bool next_page) {
  if (!ic) {
    return false;
  }

  auto candidate_list = ic->inputPanel().candidateList();
  auto* pageable = candidate_list ? candidate_list->toPageable() : nullptr;
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

class PaletteCandidateWord : public fcitx::CandidateWord {
public:
  PaletteCandidateWord(VinputEngine* engine, std::size_t index, const std::string& text,
                       const std::string& comment)
      : fcitx::CandidateWord(fcitx::Text(text)), engine_(engine), index_(index) {
    if (!comment.empty()) {
#ifdef VINPUT_FCITX5_CORE_HAVE_SET_COMMENT
      setComment(fcitx::Text(comment));
#endif
    }
  }

  void select(fcitx::InputContext* inputContext) const override {
    engine_->selectPaletteItem(index_, inputContext);
  }

private:
  VinputEngine* engine_;
  std::size_t index_;
};

class ResultCandidateWord : public fcitx::CandidateWord {
public:
  ResultCandidateWord(VinputEngine* engine, std::size_t index, const std::string& text,
                      const std::string& comment)
      : fcitx::CandidateWord(fcitx::Text(text)), engine_(engine), index_(index) {
    if (!comment.empty()) {
#ifdef VINPUT_FCITX5_CORE_HAVE_SET_COMMENT
      setComment(fcitx::Text(comment));
#endif
    }
  }

  void select(fcitx::InputContext* inputContext) const override {
    engine_->selectResultCandidate(index_, inputContext);
  }

private:
  VinputEngine* engine_;
  std::size_t index_;
};

} // namespace

void VinputEngine::reloadPaletteItems() {
  palette_items_.clear();
  const auto config = LoadCoreConfig();
  const auto i18n_map =
      vinput::registry::FetchMergedI18nMap(config, vinput::registry::DetectPreferredLocale());

  // 1. ASR items
#if VINPUT_ENABLE_LOCAL_ASR
  const auto& active_provider = config.asr.activeProvider;
  const bool is_local_active = (active_provider == "sherpa-onnx" || active_provider.empty());
  vinput::asr::ModelManager model_mgr;
  const auto local_models = model_mgr.ListDetailed(ResolvePreferredLocalModel(config));

  for (const auto& m : local_models) {
    if (m.state == vinput::asr::ModelState::Broken) {
      continue;
    }
    const bool is_active = is_local_active && (m.state == vinput::asr::ModelState::Active);
    std::string display = vinput::registry::LookupI18n(i18n_map, m.id + ".title", m.id);
    if (display.empty()) {
      display = m.id;
    }
    std::string comment = is_active ? _("[*] [ASR]") : _("[ASR]");
    palette_items_.push_back(PaletteItem{
        .category = PaletteCategory::Asr,
        .id = m.id,
        .text = display,
        .comment = std::move(comment),
        .search_text = m.id + " " + display + " asr",
        .active = is_active,
        .provider_id = "sherpa-onnx",
        .model_id = m.id,
        .scene_id = {},
        .adapter_id = {},
        .adapter_running = false,
    });
  }
#endif

  for (const auto& prov : config.asr.providers) {
    const std::string pid = AsrProviderId(prov);
    if (pid.empty() || pid == "sherpa-onnx") {
      continue;
    }
    const bool is_active = (pid == config.asr.activeProvider);
    std::string title = vinput::registry::LookupI18n(i18n_map, pid + ".title", pid);
    std::string comment = is_active ? _("[*] [ASR]") : _("[ASR]");
    palette_items_.push_back(PaletteItem{
        .category = PaletteCategory::Asr,
        .id = pid,
        .text = title,
        .comment = std::move(comment),
        .search_text = pid + " " + title + " asr",
        .active = is_active,
        .provider_id = pid,
        .model_id = {},
        .scene_id = {},
        .adapter_id = {},
        .adapter_running = false,
    });
  }

  // 2. Scene items
  for (const auto& scene : config.scenes.definitions) {
    const bool is_active = (scene.id == config.scenes.activeScene);
    const std::string label = vinput::scene::DisplayLabel(scene);
    std::string comment = is_active ? _("[*] [Scene]") : _("[Scene]");
    palette_items_.push_back(PaletteItem{
        .category = PaletteCategory::Scene,
        .id = scene.id,
        .text = label,
        .comment = std::move(comment),
        .search_text = scene.id + " " + label + " scene",
        .active = is_active,
        .provider_id = {},
        .model_id = {},
        .scene_id = scene.id,
        .adapter_id = {},
        .adapter_running = false,
    });
  }

  // 3. Command Model items
  const auto active_models = vinput::llm::LoadActiveModels(config);
  for (const auto& m : active_models) {
    std::string comment = m.active_for_command ? _("[*] [Model]") : _("[Model]");
    palette_items_.push_back(PaletteItem{
        .category = PaletteCategory::CommandModel,
        .id = m.full_id,
        .text = m.provider_id + " / " + m.model,
        .comment = std::move(comment),
        .search_text = m.full_id + " " + m.provider_id + " " + m.model + " model",
        .active = m.active_for_command,
        .provider_id = m.provider_id,
        .model_id = m.model,
        .scene_id = {},
        .adapter_id = {},
        .adapter_running = false,
    });
  }

  // 4. Adapter items
  for (const auto& adapter : config.llm.adapters) {
    const bool running = vinput::adapter::IsRunning(adapter.id);
    std::string title = vinput::registry::LookupI18n(i18n_map, adapter.id + ".title", adapter.id);
    std::string comment;
    if (running) {
      comment = adapter.autoStart ? _("[running · autostart] [Adapter]") : _("[running] [Adapter]");
    } else {
      comment = adapter.autoStart ? _("[stopped · autostart] [Adapter]") : _("[stopped] [Adapter]");
    }
    palette_items_.push_back(PaletteItem{
        .category = PaletteCategory::Adapter,
        .id = adapter.id,
        .text = title,
        .comment = std::move(comment),
        .search_text =
            adapter.id + " " + title + (running ? " running" : " stopped") + " adapter ps",
        .active = false,
        .provider_id = {},
        .model_id = {},
        .scene_id = {},
        .adapter_id = adapter.id,
        .adapter_running = running,
    });
  }
}

void VinputEngine::showPaletteMenu(fcitx::InputContext* ic, const std::string& initial_query) {
  if (!ic) {
    return;
  }

  reloadPaletteItems();
  palette_menu_ic_ = ic;
  palette_menu_visible_ = true;
  palette_query_ = initial_query;
  palette_filter_mode_ = !initial_query.empty();
  rebuildPaletteMenu(ic);
}

void VinputEngine::rebuildPaletteMenu(fcitx::InputContext* ic) {
  if (!ic) {
    return;
  }

  const ParsedPaletteQuery parsed = ParsePaletteQuery(palette_query_);
  palette_filtered_indices_.clear();

  for (std::size_t i = 0; i < palette_items_.size(); ++i) {
    const auto& item = palette_items_[i];
    if (parsed.scope.has_value() && item.category != *parsed.scope) {
      continue;
    }
    if (parsed.terms.empty() || MatchesAllTerms(item.search_text, parsed.terms)) {
      palette_filtered_indices_.push_back(i);
    }
  }

  auto candidate_list = std::make_unique<fcitx::CommonCandidateList>();
  candidate_list->setPageSize(kMenuPageSize);
  candidate_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
  candidate_list->setCursorPositionAfterPaging(fcitx::CursorPositionAfterPaging::ResetToFirst);

  for (const std::size_t idx : palette_filtered_indices_) {
    const auto& item = palette_items_[idx];
    candidate_list->append<PaletteCandidateWord>(this, idx, item.text, item.comment);
  }
  SelectFirstCandidate(candidate_list.get());

  SetMenuTitle(ic, PaletteMenuTitle(parsed, palette_filter_mode_), candidate_list.get());
  ic->inputPanel().setCandidateList(std::move(candidate_list));
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VinputEngine::resetPaletteMenuState() {
  palette_menu_ic_ = nullptr;
  palette_menu_visible_ = false;
  palette_query_.clear();
  palette_filter_mode_ = false;
  palette_filtered_indices_.clear();
}

void VinputEngine::hidePaletteMenu() {
  auto* ic = palette_menu_ic_;
  const bool was_visible = palette_menu_visible_;
  resetPaletteMenuState();

  if (!was_visible || !ic) {
    return;
  }

  fcitx::Text empty;
  ic->inputPanel().setAuxUp(empty);
  ic->inputPanel().setCandidateList({});
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

bool VinputEngine::handlePaletteMenuKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (!palette_menu_visible_ || !palette_menu_ic_) {
    return false;
  }

  auto candidate_list = palette_menu_ic_->inputPanel().candidateList();
  auto* cursor_list = candidate_list ? candidate_list->toCursorMovable() : nullptr;
  const auto normalized_key = keyEvent.key().normalize();
  const bool printable_filter_input = IsPrintableMenuInput(keyEvent.key(), palette_filter_mode_);
  const bool handled_key =
      keyEvent.key().checkKeyList(palette_menu_keys_) ||
      (!asr_menu_key_.empty() && keyEvent.key().checkKeyList(asr_menu_key_)) ||
      IsPagePrevKey(keyEvent.key()) || IsPageNextKey(keyEvent.key()) ||
      keyEvent.key().digitSelection() >= 0 || IsSlashKey(keyEvent.key()) ||
      IsBackspaceKey(keyEvent.key()) || IsCtrlShortcut(keyEvent.key(), FcitxKey_w) ||
      IsCtrlShortcut(keyEvent.key(), FcitxKey_u) || IsUpKey(keyEvent.key()) ||
      IsDownKey(keyEvent.key()) || IsEnterKey(keyEvent.key()) || IsEscapeKey(keyEvent.key()) ||
      IsPureModifierKey(keyEvent.key()) || printable_filter_input;

  if (keyEvent.isRelease()) {
    if (!handled_key) {
      return false;
    }
    keyEvent.filterAndAccept();
    return true;
  }

  if (!handled_key) {
    hidePaletteMenu();
    return false;
  }

  if (keyEvent.key().checkKeyList(palette_menu_keys_) ||
      (!asr_menu_key_.empty() && keyEvent.key().checkKeyList(asr_menu_key_)) ||
      IsPureModifierKey(keyEvent.key())) {
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsEscapeKey(keyEvent.key())) {
    if (palette_filter_mode_ || !palette_query_.empty()) {
      palette_query_.clear();
      palette_filter_mode_ = false;
      rebuildPaletteMenu(palette_menu_ic_);
    } else {
      hidePaletteMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsSlashKey(keyEvent.key())) {
    palette_filter_mode_ = true;
    palette_query_.append("/");
    rebuildPaletteMenu(palette_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsBackspaceKey(keyEvent.key()) && palette_filter_mode_) {
    if (!palette_query_.empty()) {
      PopLastUtf8Char(&palette_query_);
    } else {
      palette_filter_mode_ = false;
    }
    rebuildPaletteMenu(palette_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (palette_filter_mode_ && IsCtrlShortcut(keyEvent.key(), FcitxKey_w)) {
    DeleteLastWord(&palette_query_);
    if (palette_query_.empty()) {
      palette_filter_mode_ = false;
    }
    rebuildPaletteMenu(palette_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (palette_filter_mode_ && IsCtrlShortcut(keyEvent.key(), FcitxKey_u)) {
    palette_query_.clear();
    palette_filter_mode_ = false;
    rebuildPaletteMenu(palette_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (printable_filter_input) {
    const std::string utf8 = fcitx::Key::keySymToUTF8(normalized_key.sym());
    palette_query_.append(utf8);
    rebuildPaletteMenu(palette_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsPagePrevKey(keyEvent.key())) {
    const ParsedPaletteQuery parsed = ParsePaletteQuery(palette_query_);
    ChangeCandidatePage(palette_menu_ic_, PaletteMenuTitle(parsed, palette_filter_mode_), false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsPageNextKey(keyEvent.key())) {
    const ParsedPaletteQuery parsed = ParsePaletteQuery(palette_query_);
    ChangeCandidatePage(palette_menu_ic_, PaletteMenuTitle(parsed, palette_filter_mode_), true);
    keyEvent.filterAndAccept();
    return true;
  }

  const int digit = keyEvent.key().digitSelection();
  const int digit_index = DigitSelectionIndex(candidate_list.get(), digit);
  if (digit >= 0 && digit_index < static_cast<int>(palette_filtered_indices_.size())) {
    selectPaletteItem(palette_filtered_indices_[digit_index], palette_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && IsUpKey(keyEvent.key())) {
    cursor_list->prevCandidate();
    palette_menu_ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && IsDownKey(keyEvent.key())) {
    cursor_list->nextCandidate();
    palette_menu_ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsEnterKey(keyEvent.key())) {
    int index = CurrentSelectionIndex(candidate_list.get());
    if (index < 0) {
      index = palette_filtered_indices_.empty() ? -1 : 0;
    }
    if (index >= 0 && index < static_cast<int>(palette_filtered_indices_.size())) {
      selectPaletteItem(palette_filtered_indices_[index], palette_menu_ic_);
    } else {
      hidePaletteMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  hidePaletteMenu();
  return false;
}

void VinputEngine::selectPaletteItem(std::size_t index, fcitx::InputContext* /*ic*/) {
  if (index >= palette_items_.size()) {
    hidePaletteMenu();
    return;
  }

  const auto item = palette_items_[index];
  hidePaletteMenu();

  switch (item.category) {
  case PaletteCategory::Asr: {
    auto core_config = LoadCoreConfig();
    core_config.asr.activeProvider = item.provider_id;
    if (!item.model_id.empty()) {
      std::string error;
      if (!SetPreferredLocalModel(&core_config, item.model_id, &error)) {
        notifyError(error);
        return;
      }
    }
    if (!SaveCoreConfig(core_config)) {
      notifyError(_("Failed to save ASR config."));
      return;
    }
    if (!queryDaemonStatus().empty()) {
      std::string reload_error;
      if (!callReloadAsrBackend(&reload_error)) {
        notifyError(reload_error.empty() ? _("Failed to reload ASR backend.") : reload_error);
        return;
      }
    }
    notifyInfo(vinput::str::FmtStr(_("ASR switch requested for '%s'."), item.text.c_str()));
    break;
  }
  case PaletteCategory::Scene: {
    auto core_config = LoadCoreConfig();
    core_config.scenes.activeScene = item.scene_id;
    if (!SaveCoreConfig(core_config)) {
      notifyError(_("Failed to save scene config."));
      return;
    }
    active_scene_id_ = item.scene_id;
    notifyInfo(vinput::str::FmtStr(_("Scene switched to '%s'."), item.text.c_str()));
    break;
  }
  case PaletteCategory::CommandModel: {
    auto core_config = LoadCoreConfig();
    std::string error;
    if (!vinput::llm::SetActiveCommandModel(&core_config, item.provider_id, item.model_id,
                                            &error)) {
      notifyError(error.empty() ? _("Failed to set command model.") : error);
      return;
    }
    notifyInfo(vinput::str::FmtStr(_("Command model set to '%s / %s'."), item.provider_id.c_str(),
                                   item.model_id.c_str()));
    break;
  }
  case PaletteCategory::Adapter: {
    std::string error;
    if (item.adapter_running) {
      if (!callStopAdapter(item.adapter_id, &error)) {
        notifyError(error.empty() ? _("Failed to stop adapter.") : error);
        return;
      }
      notifyInfo(vinput::str::FmtStr(_("Adapter '%s' stopped."), item.text.c_str()));
    } else {
      if (!callStartAdapter(item.adapter_id, &error)) {
        notifyError(error.empty() ? _("Failed to start adapter.") : error);
        return;
      }
      notifyInfo(vinput::str::FmtStr(_("Adapter '%s' started."), item.text.c_str()));
    }
    break;
  }
  }
}

void VinputEngine::showResultMenu(fcitx::InputContext* ic, const vinput::result::Payload& payload) {
  if (!ic || payload.candidates.empty()) {
    return;
  }

  hidePaletteMenu();
  result_menu_ic_ = ic;
  result_menu_visible_ = true;
  result_candidates_ = payload.candidates;

  auto candidate_list = std::make_unique<fcitx::CommonCandidateList>();
  candidate_list->setPageSize(instance_->globalConfig().defaultPageSize());
  candidate_list->setSelectionKey(fcitx::Key::keyListFromString("1 2 3 4 5 6 7 8 9 0"));
  candidate_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
  candidate_list->setCursorPositionAfterPaging(fcitx::CursorPositionAfterPaging::ResetToFirst);

  int cursor_index = 0;
  for (std::size_t i = 0; i < result_candidates_.size(); ++i) {
    const auto& candidate = result_candidates_[i];
    if (candidate.text == payload.commitText) {
      cursor_index = static_cast<int>(i);
    }
    candidate_list->append<ResultCandidateWord>(this, i, candidate.text,
                                                ResultCandidateComment(candidate));
  }
  MoveCursorToIndex(candidate_list.get(), cursor_index);

  SetMenuTitle(ic, ResultMenuTitle(result_candidates_.size()), candidate_list.get());
  ic->inputPanel().setCandidateList(std::move(candidate_list));
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VinputEngine::resetResultMenuState() {
  result_menu_ic_ = nullptr;
  result_menu_visible_ = false;
  result_candidates_.clear();
}

void VinputEngine::hideResultMenu() {
  auto* ic = result_menu_ic_;
  const bool was_visible = result_menu_visible_;
  resetResultMenuState();

  if (!was_visible || !ic) {
    return;
  }

  fcitx::Text empty;
  ic->inputPanel().setAuxUp(empty);
  ic->inputPanel().setCandidateList({});
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

bool VinputEngine::handleResultMenuKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (!result_menu_visible_ || !result_menu_ic_) {
    return false;
  }

  auto candidate_list = result_menu_ic_->inputPanel().candidateList();
  auto* cursor_list = candidate_list ? candidate_list->toCursorMovable() : nullptr;
  if (keyEvent.isRelease()) {
    if (keyEvent.key().digitSelection() >= 0 || keyEvent.key().checkKeyList(page_prev_keys_) ||
        keyEvent.key().checkKeyList(page_next_keys_) || keyEvent.key().check(FcitxKey_Up) ||
        keyEvent.key().check(FcitxKey_Down) || keyEvent.key().check(FcitxKey_Return) ||
        keyEvent.key().check(FcitxKey_KP_Enter) || keyEvent.key().check(FcitxKey_Escape)) {
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
    ChangeCandidatePage(result_menu_ic_, ResultMenuTitle(result_candidates_.size()), false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().checkKeyList(page_next_keys_)) {
    ChangeCandidatePage(result_menu_ic_, ResultMenuTitle(result_candidates_.size()), true);
    keyEvent.filterAndAccept();
    return true;
  }

  const int digit = keyEvent.key().digitSelection();
  if (digit >= 0 && digit < candidate_list->size()) {
    candidate_list->candidate(digit).select(result_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && keyEvent.key().check(FcitxKey_Up)) {
    cursor_list->prevCandidate();
    result_menu_ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (cursor_list && keyEvent.key().check(FcitxKey_Down)) {
    cursor_list->nextCandidate();
    result_menu_ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    keyEvent.filterAndAccept();
    return true;
  }

  if (keyEvent.key().check(FcitxKey_Return) || keyEvent.key().check(FcitxKey_KP_Enter)) {
    int index = candidate_list->cursorIndex();
    if (index < 0) {
      index = 0;
    }
    if (index >= 0 && index < candidate_list->size()) {
      candidate_list->candidate(index).select(result_menu_ic_);
    } else {
      hideResultMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  hideResultMenu();
  return false;
}

void VinputEngine::selectResultCandidate(std::size_t index, fcitx::InputContext* ic) {
  if (index >= result_candidates_.size() || !ic) {
    hideResultMenu();
    return;
  }

  const auto chosen = result_candidates_[index];
  hideResultMenu();

  if (chosen.source == vinput::result::kSourceLlm) {
    appendContextEntry(chosen.text, "llm");
  }
  suppressNextCommitContext(chosen.text);

  if (result_is_command_) {
    auto& surrounding = ic->surroundingText();
    if (surrounding.isValid() && surrounding.cursor() != surrounding.anchor()) {
      int cursor = surrounding.cursor();
      int anchor = surrounding.anchor();
      int from = std::min(cursor, anchor);
      int to = std::max(cursor, anchor);
      ic->deleteSurroundingText(from - cursor, to - from);
    }
    result_is_command_ = false;
  }

  ic->commitString(chosen.text);
}
