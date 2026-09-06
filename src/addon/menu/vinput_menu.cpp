#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fcitx-utils/i18n.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "config.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include "common/asr/model_manager.h"
#endif
#include "common/asr/recognition_result.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/llm/adapter_manager.h"
#include "common/llm/provider_model_cache.h"
#include "common/registry/registry_i18n.h"
#include "common/runtime/runtime_defaults.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/string_utils.h"

#include "core/vinput.h"
#include "menu/palette_command.h"

namespace {

constexpr int kMenuPageSize = 10;

struct ParsedPaletteQuery {
  const PaletteCommand* command = nullptr;
  std::string terms;
  bool is_command_mode = false;
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
  if (text == nullptr || text->empty()) {
    return;
  }

  std::size_t pos = text->size();
  do {
    --pos;
  } while (pos > 0 && (static_cast<unsigned char>((*text)[pos]) & 0xC0) == 0x80);
  text->erase(pos);
}

ParsedPaletteQuery ParsePaletteQuery(std::string_view raw_query,
                                     const PaletteCommandRegistry& registry) {
  ParsedPaletteQuery res;
  std::string_view q = raw_query;
  while (!q.empty() && std::isspace(static_cast<unsigned char>(q.front())) != 0) {
    q.remove_prefix(1);
  }

  if (q.empty()) {
    return res;
  }

  if (q.front() == '/') {
    res.is_command_mode = true;
    q.remove_prefix(1);

    std::size_t token_end = 0;
    while (token_end < q.size() && std::isspace(static_cast<unsigned char>(q[token_end])) == 0 &&
           q[token_end] != '/') {
      ++token_end;
    }

    const std::string_view token = q.substr(0, token_end);
    if (!token.empty()) {
      res.command = registry.findByNameOrAlias(token);
    }
    q.remove_prefix(token_end);
  }

  while (!q.empty() &&
         (std::isspace(static_cast<unsigned char>(q.front())) != 0 || q.front() == '/')) {
    q.remove_prefix(1);
  }
  res.terms = std::string(q);
  return res;
}

std::string PaletteMenuTitle(const ParsedPaletteQuery& parsed) {
  if (parsed.command != nullptr) {
    if (!parsed.terms.empty()) {
      return parsed.command->title + ": " + parsed.terms;
    }
    return parsed.command->title;
  }

  if (parsed.is_command_mode) {
    return _("Command Palette");
  }

  if (!parsed.terms.empty()) {
    return vinput::str::FmtStr(_("Search: %s"), parsed.terms.c_str());
  }

  return _("Command Palette");
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
  if (ic == nullptr) {
    return;
  }

  const fcitx::Text title(DecoratePagedMenuTitle(base_title, candidate_list));
  ic->inputPanel().setAuxUp(title);
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

bool IsBackspaceKey(const fcitx::Key& key) {
  return IsKeySym(key, FcitxKey_BackSpace);
}

bool IsUpKey(const fcitx::Key& key) {
  return IsKeySym(key, FcitxKey_Up);
}

bool IsDownKey(const fcitx::Key& key) {
  return IsKeySym(key, FcitxKey_Down);
}

bool IsPrintableMenuInput(const fcitx::Key& key) {
  if (IsEnterKey(key) || IsEscapeKey(key) || IsBackspaceKey(key) || IsUpKey(key) ||
      IsDownKey(key) || IsPagePrevKey(key) || IsPageNextKey(key) || IsPureModifierKey(key)) {
    return false;
  }

  if (key.digitSelection() >= 0) {
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

void VinputEngine::initializePaletteRegistry() {
  palette_registry_.clear();

  // 1. Model (/model, alias /m)
  palette_registry_.registerCommand(PaletteCommand{
      .id = "model",
      .name = "model",
      .alias = "m",
      .title = _("Cmd-Model"),
      .comment = _("Cmd-Model"),
      .description = _("Switch command model"),
      .category = PaletteCategory::CommandModel,
      .getItems =
          [](VinputEngine* /*engine*/) {
            std::vector<PaletteItem> items;
            const auto config = LoadCoreConfig();
            const auto active_models = vinput::llm::LoadActiveModels(config);
            for (const auto& m : active_models) {
              std::string comment = m.active_for_command ? _("Cmd-Model *") : _("Cmd-Model");
              items.push_back(PaletteItem{
                  .category = PaletteCategory::CommandModel,
                  .id = m.full_id,
                  .text = m.provider_id + " / " + m.model,
                  .comment = std::move(comment),
                  .search_text = m.full_id + " " + m.provider_id + " " + m.model + " model cmd",
                  .active = m.active_for_command,
                  .provider_id = m.provider_id,
                  .model_id = m.model,
                  .scene_id = {},
                  .adapter_id = {},
                  .adapter_running = false,
                  .is_command_entry = false,
                  .command_token = {},
              });
            }
            return items;
          },
      .onSelect =
          [](VinputEngine* engine, const PaletteItem& item, fcitx::InputContext* /*ic*/) {
            auto core_config = LoadCoreConfig();
            std::string error;
            if (!vinput::llm::SetActiveCommandModel(&core_config, item.provider_id, item.model_id,
                                                    &error)) {
              engine->notifyError(error.empty() ? _("Failed to set command model.") : error);
              return;
            }
            engine->notifyInfo(vinput::str::FmtStr(_("Command model set to '%s / %s'."),
                                                   item.provider_id.c_str(),
                                                   item.model_id.c_str()));
          },
  });

  // 2. ASR (/asr, alias /a)
  palette_registry_.registerCommand(PaletteCommand{
      .id = "asr",
      .name = "asr",
      .alias = "a",
      .title = _("ASR"),
      .comment = _("ASR"),
      .description = _("Switch ASR provider or model"),
      .category = PaletteCategory::Asr,
      .getItems =
          [](VinputEngine* /*engine*/) {
            std::vector<PaletteItem> items;
            const auto config = LoadCoreConfig();
            const auto i18n_map = vinput::registry::LoadMergedCachedI18nMap(
                vinput::registry::DetectPreferredLocale(), nullptr);

#if VINPUT_ENABLE_LOCAL_ASR
            const auto& active_provider = config.asr.activeProvider;
            const bool is_local_active =
                (active_provider == "sherpa-onnx" || active_provider.empty());
            const ModelManager model_mgr;
            const auto local_models = model_mgr.ListDetailed(ResolvePreferredLocalModel(config));

            for (const auto& m : local_models) {
              if (m.state == ModelState::Broken) {
                continue;
              }
              const bool is_active = is_local_active && (m.state == ModelState::Active);
              std::string display = vinput::registry::LookupI18n(i18n_map, m.id + ".title", m.id);
              if (display.empty()) {
                display = m.id;
              }
              std::string comment = is_active ? _("ASR *") : _("ASR");
              items.push_back(PaletteItem{
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
                  .is_command_entry = false,
                  .command_token = {},
              });
            }
#endif

            for (const auto& prov : config.asr.providers) {
              if (std::holds_alternative<LocalAsrProvider>(prov)) {
                continue;
              }
              const std::string pid = AsrProviderId(prov);
              if (pid.empty()) {
                continue;
              }
              const bool is_active = (pid == config.asr.activeProvider);
              const std::string title = vinput::registry::LookupI18n(i18n_map, pid + ".title", pid);
              std::string comment = is_active ? _("ASR *") : _("ASR");
              std::string search_text = pid;
              search_text += ' ';
              search_text += title;
              search_text += " asr";
              items.push_back(PaletteItem{
                  .category = PaletteCategory::Asr,
                  .id = pid,
                  .text = title,
                  .comment = std::move(comment),
                  .search_text = std::move(search_text),
                  .active = is_active,
                  .provider_id = pid,
                  .model_id = {},
                  .scene_id = {},
                  .adapter_id = {},
                  .adapter_running = false,
                  .is_command_entry = false,
                  .command_token = {},
              });
            }
            return items;
          },
      .onSelect =
          [](VinputEngine* engine, const PaletteItem& item, fcitx::InputContext* /*ic*/) {
            auto core_config = LoadCoreConfig();
#if !VINPUT_ENABLE_LOCAL_ASR
            const auto* target_provider = ResolveAsrProvider(core_config, item.provider_id);
            if (target_provider && std::holds_alternative<LocalAsrProvider>(*target_provider)) {
              engine->notifyError(
                  vinput::str::FmtStr(_("ASR provider '%s' is a local provider, which is not "
                                        "supported in this Lite build."),
                                      item.provider_id.c_str()));
              return;
            }
#endif
            core_config.asr.activeProvider = item.provider_id;
            if (!item.model_id.empty()) {
              std::string error;
              if (!SetPreferredLocalModel(&core_config, item.model_id, &error)) {
                engine->notifyError(error);
                return;
              }
            }
            if (!SaveCoreConfig(core_config)) {
              engine->notifyError(_("Failed to save ASR config."));
              return;
            }
            if (!engine->queryDaemonStatus().empty()) {
              std::string reload_error;
              if (!engine->callReloadAsrBackend(&reload_error)) {
                engine->notifyError(reload_error.empty() ? _("Failed to reload ASR backend.")
                                                         : reload_error);
                return;
              }
            }
            engine->notifyInfo(
                vinput::str::FmtStr(_("ASR switch requested for '%s'."), item.text.c_str()));
          },
  });

  // 3. Scene (/scene, alias /s)
  palette_registry_.registerCommand(PaletteCommand{
      .id = "scene",
      .name = "scene",
      .alias = "s",
      .title = _("Scene"),
      .comment = _("Scene"),
      .description = _("Switch dictation scene"),
      .category = PaletteCategory::Scene,
      .getItems =
          [](VinputEngine* /*engine*/) {
            std::vector<PaletteItem> items;
            const auto config = LoadCoreConfig();
            for (const auto& scene : config.scenes.definitions) {
              const bool is_active = (scene.id == config.scenes.activeScene);
              const std::string label = vinput::scene::DisplayLabel(scene);
              std::string comment = is_active ? _("Scene *") : _("Scene");
              items.push_back(PaletteItem{
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
                  .is_command_entry = false,
                  .command_token = {},
              });
            }
            return items;
          },
      .onSelect =
          [](VinputEngine* engine, const PaletteItem& item, fcitx::InputContext* /*ic*/) {
            auto core_config = LoadCoreConfig();
            core_config.scenes.activeScene = item.scene_id;
            if (!SaveCoreConfig(core_config)) {
              engine->notifyError(_("Failed to save scene config."));
              return;
            }
            engine->active_scene_id_ = item.scene_id;
            engine->notifyInfo(
                vinput::str::FmtStr(_("Scene switched to '%s'."), item.text.c_str()));
          },
  });

  // 4. Proc (/proc, alias /p)
  palette_registry_.registerCommand(PaletteCommand{
      .id = "proc",
      .name = "proc",
      .alias = "p",
      .title = _("Proc"),
      .comment = _("Proc"),
      .description = _("Manage adapter processes"),
      .category = PaletteCategory::Adapter,
      .getItems =
          [](VinputEngine* /*engine*/) {
            std::vector<PaletteItem> items;
            const auto config = LoadCoreConfig();
            const auto i18n_map = vinput::registry::LoadMergedCachedI18nMap(
                vinput::registry::DetectPreferredLocale(), nullptr);
            for (const auto& adapter : config.llm.adapters) {
              const bool running = vinput::adapter::IsRunning(adapter.id);
              const std::string title =
                  vinput::registry::LookupI18n(i18n_map, adapter.id + ".title", adapter.id);
              const bool is_autostart = adapter.autoStart;
              std::string comment;
              if (running) {
                comment = is_autostart ? _("Proc (running · autostart)") : _("Proc (running)");
              } else {
                comment = is_autostart ? _("Proc (stopped · autostart)") : _("Proc (stopped)");
              }
              items.push_back(PaletteItem{
                  .category = PaletteCategory::Adapter,
                  .id = adapter.id,
                  .text = title,
                  .comment = std::move(comment),
                  .search_text = adapter.id + " " + title + (running ? " running" : " stopped") +
                                 " adapter proc ps",
                  .active = false,
                  .provider_id = {},
                  .model_id = {},
                  .scene_id = {},
                  .adapter_id = adapter.id,
                  .adapter_running = running,
                  .is_command_entry = false,
                  .command_token = {},
              });
            }
            return items;
          },
      .onSelect =
          [](VinputEngine* engine, const PaletteItem& item, fcitx::InputContext* /*ic*/) {
            std::string error;
            if (item.adapter_running) {
              if (!engine->callStopAdapter(item.adapter_id, &error)) {
                engine->notifyError(error.empty() ? _("Failed to stop adapter.") : error);
                return;
              }
              engine->notifyInfo(
                  vinput::str::FmtStr(_("Adapter '%s' stopped."), item.text.c_str()));
            } else {
              if (!engine->callStartAdapter(item.adapter_id, &error)) {
                engine->notifyError(error.empty() ? _("Failed to start adapter.") : error);
                return;
              }
              engine->notifyInfo(
                  vinput::str::FmtStr(_("Adapter '%s' started."), item.text.c_str()));
            }
          },
  });
}

void VinputEngine::reloadPaletteItems() {
  palette_items_.clear();

  for (const auto& cmd : palette_registry_.commands()) {
    std::string text = "/" + cmd.name;
    if (!cmd.alias.empty()) {
      text += " (/" + cmd.alias + ")";
    }
    std::string search_text = cmd.name + " " + cmd.alias + " " + cmd.description;
    palette_items_.push_back(PaletteItem{
        .category = cmd.category,
        .id = "cmd:" + cmd.id,
        .text = std::move(text),
        .comment = cmd.description,
        .search_text = std::move(search_text),
        .active = false,
        .provider_id = {},
        .model_id = {},
        .scene_id = {},
        .adapter_id = {},
        .adapter_running = false,
        .is_command_entry = true,
        .command_token = cmd.alias.empty() ? cmd.name : cmd.alias,
    });
  }

  for (const auto& cmd : palette_registry_.commands()) {
    if (cmd.getItems) {
      auto items = cmd.getItems(this);
      palette_items_.insert(palette_items_.end(), std::make_move_iterator(items.begin()),
                            std::make_move_iterator(items.end()));
    }
  }
}

void VinputEngine::showPaletteMenu(fcitx::InputContext* ic, const std::string& initial_query) {
  if (ic == nullptr) {
    return;
  }

  reloadPaletteItems();
  palette_menu_ic_ = ic;
  palette_menu_visible_ = true;
  palette_query_ = initial_query;
  rebuildPaletteMenu(ic);
}

void VinputEngine::rebuildPaletteMenu(fcitx::InputContext* ic) {
  if (ic == nullptr) {
    return;
  }

  const ParsedPaletteQuery parsed = ParsePaletteQuery(palette_query_, palette_registry_);
  palette_filtered_indices_.clear();

  for (std::size_t i = 0; i < palette_items_.size(); ++i) {
    const auto& item = palette_items_[i];

    if (item.is_command_entry) {
      if (palette_query_.empty()) {
        palette_filtered_indices_.push_back(i);
      } else if (parsed.is_command_mode && parsed.command == nullptr) {
        if (parsed.terms.empty() || MatchesAllTerms(item.search_text, parsed.terms)) {
          palette_filtered_indices_.push_back(i);
        }
      }
      continue;
    }

    if (palette_query_.empty()) {
      continue;
    }

    if (parsed.command != nullptr) {
      if (item.category != parsed.command->category) {
        continue;
      }
      if (parsed.terms.empty() || MatchesAllTerms(item.search_text, parsed.terms)) {
        palette_filtered_indices_.push_back(i);
      }
    } else if (!parsed.is_command_mode) {
      if (MatchesAllTerms(item.search_text, parsed.terms)) {
        palette_filtered_indices_.push_back(i);
      }
    }
  }

  auto candidate_list = std::make_unique<fcitx::CommonCandidateList>();
  candidate_list->setPageSize(instance_->globalConfig().defaultPageSize());
  candidate_list->setSelectionKey(fcitx::Key::keyListFromString("1 2 3 4 5 6 7 8 9 0"));
  candidate_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
  candidate_list->setCursorPositionAfterPaging(fcitx::CursorPositionAfterPaging::ResetToFirst);

  for (const std::size_t idx : palette_filtered_indices_) {
    const auto& item = palette_items_[idx];
    candidate_list->append<PaletteCandidateWord>(this, idx, item.text, item.comment);
  }
  SelectFirstCandidate(candidate_list.get());

  SetMenuTitle(ic, PaletteMenuTitle(parsed), candidate_list.get());
  ic->inputPanel().setCandidateList(std::move(candidate_list));
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VinputEngine::resetPaletteMenuState() {
  palette_menu_ic_ = nullptr;
  palette_menu_visible_ = false;
  palette_query_.clear();
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
  if (!palette_menu_visible_ || palette_menu_ic_ == nullptr) {
    return false;
  }

  auto candidate_list = palette_menu_ic_->inputPanel().candidateList();
  auto* cursor_list = candidate_list ? candidate_list->toCursorMovable() : nullptr;
  const auto normalized_key = keyEvent.key().normalize();
  const bool printable_filter_input = IsPrintableMenuInput(keyEvent.key());
  const bool handled_key = IsPagePrevKey(keyEvent.key()) || IsPageNextKey(keyEvent.key()) ||
                           keyEvent.key().digitSelection() >= 0 || IsBackspaceKey(keyEvent.key()) ||
                           IsUpKey(keyEvent.key()) || IsDownKey(keyEvent.key()) ||
                           IsEnterKey(keyEvent.key()) || IsEscapeKey(keyEvent.key()) ||
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

  if (IsPureModifierKey(keyEvent.key())) {
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsEscapeKey(keyEvent.key())) {
    if (!palette_query_.empty()) {
      palette_query_.clear();
      rebuildPaletteMenu(palette_menu_ic_);
    } else {
      hidePaletteMenu();
    }
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsBackspaceKey(keyEvent.key())) {
    if (!palette_query_.empty()) {
      PopLastUtf8Char(&palette_query_);
      rebuildPaletteMenu(palette_menu_ic_);
    } else {
      hidePaletteMenu();
    }
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
    const ParsedPaletteQuery parsed = ParsePaletteQuery(palette_query_, palette_registry_);
    ChangeCandidatePage(palette_menu_ic_, PaletteMenuTitle(parsed), false);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsPageNextKey(keyEvent.key())) {
    const ParsedPaletteQuery parsed = ParsePaletteQuery(palette_query_, palette_registry_);
    ChangeCandidatePage(palette_menu_ic_, PaletteMenuTitle(parsed), true);
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

void VinputEngine::selectPaletteItem(std::size_t index, fcitx::InputContext* ic) {
  if (index >= palette_items_.size()) {
    hidePaletteMenu();
    return;
  }

  const auto item = palette_items_[index];

  if (item.is_command_entry) {
    palette_query_ = "/" + item.command_token + " ";
    rebuildPaletteMenu(palette_menu_ic_ != nullptr ? palette_menu_ic_ : ic);
    return;
  }

  hidePaletteMenu();

  const auto* cmd = palette_registry_.findByCategory(item.category);
  if (cmd != nullptr && cmd->onSelect) {
    cmd->onSelect(this, item, ic);
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
  if (index >= result_candidates_.size() || ic == nullptr) {
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
      const auto cursor = static_cast<int>(surrounding.cursor());
      const auto anchor = static_cast<int>(surrounding.anchor());
      const int from = std::min(cursor, anchor);
      const int to = std::max(cursor, anchor);
      ic->deleteSurroundingText(from - cursor, to - from);
    }
    result_is_command_ = false;
  }

  ic->commitString(chosen.text);
}
