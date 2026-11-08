#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "config.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include "common/asr/model_manager.h"
#endif
#include <fmt/printf.h>

#include "common/asr/recognition_result.h"
#include "common/client/client_manager.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/dbus/dbus_interface.h"
#include "common/llm/provider_model_cache.h"
#include "common/registry/registry_i18n.h"
#include "common/runtime/runtime_defaults.h"
#include "common/scene/postprocess_scene.h"

#include "core/vinput.h"
#include "menu/palette_command.h"

namespace {

struct ParsedPaletteQuery {
  const PaletteCommand* command = nullptr;
  std::string terms;
  bool is_command_mode = false;
};

std::string NormalizeSearchText(std::string text) {
  std::ranges::transform(text, text.begin(),
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
  const auto terms = SplitSearchTerms(query);
  return std::ranges::all_of(terms, [&](const std::string& term) {
    return normalized_haystack.find(term) != std::string::npos;
  });
}

void PopLastUtf8Char(std::string* text) {
  if (text == nullptr || text->empty()) {
    return;
  }

  std::size_t pos = text->size();
  while (pos > 0) {
    --pos;
    if ((static_cast<unsigned char>((*text)[pos]) & 0xC0) != 0x80) {
      break;
    }
  }
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
    return fmt::sprintf(_("Search: %s"), parsed.terms.c_str());
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
  auto* pageable = candidate_list != nullptr ? candidate_list->toPageable() : nullptr;
  if (pageable == nullptr) {
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
  return std::ranges::any_of(syms, [&](fcitx::KeySym sym) { return IsKeySym(key, sym); });
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

  return std::ranges::all_of(utf8, [](unsigned char ch) { return ch >= 0x20 && ch != 0x7f; });
}

void SelectFirstCandidate(fcitx::CommonCandidateList* candidate_list) {
  if (candidate_list == nullptr || candidate_list->totalSize() <= 0) {
    return;
  }

  candidate_list->setGlobalCursorIndex(0);
}

bool ChangeCandidatePage(fcitx::InputContext* ic, const std::string& base_title, bool next_page) {
  if (ic == nullptr) {
    return false;
  }

  auto candidate_list = ic->inputPanel().candidateList();
  auto* pageable = candidate_list != nullptr ? candidate_list->toPageable() : nullptr;
  if (pageable == nullptr) {
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

bool IsPaletteItemVisible(const PaletteItem& item, const ParsedPaletteQuery& parsed,
                          std::string_view query) {
  if (item.is_command_entry) {
    if (query.empty()) {
      return true;
    }
    return parsed.is_command_mode && parsed.command == nullptr &&
           (parsed.terms.empty() || MatchesAllTerms(item.search_text, parsed.terms));
  }

  if (query.empty()) {
    return false;
  }

  if (parsed.command != nullptr) {
    return (item.category == parsed.command->category) &&
           (parsed.terms.empty() || MatchesAllTerms(item.search_text, parsed.terms));
  }

  return !parsed.is_command_mode && MatchesAllTerms(item.search_text, parsed.terms);
}

bool IsHandledPaletteMenuKey(const fcitx::Key& key, bool printable) {
  return IsPagePrevKey(key) || IsPageNextKey(key) || key.digitSelection() >= 0 ||
         IsBackspaceKey(key) || IsUpKey(key) || IsDownKey(key) || IsEnterKey(key) ||
         IsEscapeKey(key) || IsPureModifierKey(key) || printable;
}

bool HandlePaletteQueryEditing(const fcitx::Key& key, bool printable, std::string* query) {
  if (query == nullptr) {
    return false;
  }
  if (IsEscapeKey(key)) {
    if (!query->empty()) {
      query->clear();
      return true;
    }
    return false;
  }
  if (IsBackspaceKey(key)) {
    if (!query->empty()) {
      PopLastUtf8Char(query);
      return true;
    }
    return false;
  }
  if (printable) {
    const std::string utf8 = fcitx::Key::keySymToUTF8(key.normalize().sym());
    query->append(utf8);
    return true;
  }
  return false;
}

bool HandlePaletteCandidateAction(const fcitx::Key& key, fcitx::CandidateList* list,
                                  fcitx::InputContext* ic) {
  if (list == nullptr) {
    return false;
  }
  const int digit = key.digitSelection();
  if (digit >= 0 && digit < list->size()) {
    list->candidate(digit).select(ic);
    return true;
  }
  auto* cursor_list = list->toCursorMovable();
  if (cursor_list != nullptr && IsUpKey(key)) {
    cursor_list->prevCandidate();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    return true;
  }
  if (cursor_list != nullptr && IsDownKey(key)) {
    cursor_list->nextCandidate();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    return true;
  }
  if (IsEnterKey(key)) {
    if (list->empty()) {
      return false;
    }
    const int index = std::max(list->cursorIndex(), 0);
    if (index >= 0 && index < list->size()) {
      list->candidate(index).select(ic);
      return true;
    }
    return false;
  }
  return false;
}

bool IsHandledResultMenuKey(const fcitx::Key& key, const std::vector<fcitx::Key>& prev_keys,
                            const std::vector<fcitx::Key>& next_keys) {
  return key.digitSelection() >= 0 || key.checkKeyList(prev_keys) || key.checkKeyList(next_keys) ||
         key.check(FcitxKey_Up) || key.check(FcitxKey_Down) || key.check(FcitxKey_Return) ||
         key.check(FcitxKey_KP_Enter) || key.check(FcitxKey_Escape);
}

bool HandleResultCandidateAction(const fcitx::Key& key, fcitx::CandidateList* list,
                                 fcitx::InputContext* ic) {
  if (list == nullptr) {
    return false;
  }
  const int digit = key.digitSelection();
  if (digit >= 0 && digit < list->size()) {
    list->candidate(digit).select(ic);
    return true;
  }
  auto* cursor_list = list->toCursorMovable();
  if (cursor_list != nullptr && key.check(FcitxKey_Up)) {
    cursor_list->prevCandidate();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    return true;
  }
  if (cursor_list != nullptr && key.check(FcitxKey_Down)) {
    cursor_list->nextCandidate();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    return true;
  }
  if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
    if (list->empty()) {
      return false;
    }
    const int index = std::max(list->cursorIndex(), 0);
    if (index >= 0 && index < list->size()) {
      list->candidate(index).select(ic);
      return true;
    }
    return false;
  }
  return false;
}

#if VINPUT_ENABLE_LOCAL_ASR
void AppendLocalAsrItems(const CoreConfig& config,
                         const std::unordered_map<std::string, std::string>& i18n_map,
                         std::vector<PaletteItem>* items) {
  if (items == nullptr) {
    return;
  }
  const auto& active_provider = config.asr.activeProvider;
  const bool is_local_active = (active_provider == "sherpa-onnx" || active_provider.empty());
  const ModelManager model_mgr;
  const auto local_models = model_mgr.ListDetailed(CoreConfigView(config).PreferredLocalModel());

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
    items->push_back(PaletteItem{
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
        .is_command_entry = false,
        .command_token = {},
    });
  }
}
#endif

void AppendCloudAsrItems(const CoreConfig& config,
                         const std::unordered_map<std::string, std::string>& i18n_map,
                         std::vector<PaletteItem>* items) {
  if (items == nullptr) {
    return;
  }
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
    items->push_back(PaletteItem{
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
        .is_command_entry = false,
        .command_token = {},
    });
  }
}

std::vector<PaletteItem> LoadAsrItems() {
  std::vector<PaletteItem> items;
  const auto config = LoadCoreConfig();
  const auto i18n_map =
      vinput::registry::LoadMergedCachedI18nMap(vinput::registry::DetectPreferredLocale(), nullptr);
#if VINPUT_ENABLE_LOCAL_ASR
  AppendLocalAsrItems(config, i18n_map, &items);
#endif
  AppendCloudAsrItems(config, i18n_map, &items);
  return items;
}

std::string FormatClientComment(bool running, pid_t pid, bool auto_start) {
  std::string status = _("stopped");
  if (running) {
    status = (pid > 0) ? fmt::sprintf(_("running (%d)"), pid) : _("running");
  }
  if (auto_start) {
    status += _(" [autostart]");
  }
  return status;
}

PaletteItem BuildClientPaletteItem(const ClientSpec& client, bool is_trigger, bool running,
                                   pid_t pid) {
  const std::string type_label = is_trigger ? _("[Trigger]") : _("[Receiver]");
  std::string text = fmt::sprintf("%s %s", type_label.c_str(), client.id.c_str());
  std::string comment = FormatClientComment(running, pid, client.autoStart);

  std::string search_text = client.id;
  search_text += ' ';
  search_text += client.command;
  search_text += is_trigger ? " trigger " : " receiver ";
  if (client.autoStart) {
    search_text += "autostart ";
  }
  search_text += running ? "running " : "stopped ";
  search_text += "proc process";

  return PaletteItem{
      .category = PaletteCategory::Process,
      .id = client.id,
      .text = std::move(text),
      .comment = std::move(comment),
      .search_text = std::move(search_text),
      .active = running,
      .provider_id = {},
      .model_id = {},
      .scene_id = {},
      .adapter_id = {},
      .is_command_entry = false,
      .command_token = {},
  };
}

} // namespace

void VinputEngine::registerModelCommand() {
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
                  .is_command_entry = false,
                  .command_token = {},
              });
            }
            return items;
          },
      .onSelect =
          [this](VinputEngine* /*engine*/, const PaletteItem& item, fcitx::InputContext* /*ic*/) {
            auto core_config = LoadCoreConfig();
            std::string error;
            if (!vinput::llm::SetActiveCommandModel(&core_config, item.provider_id, item.model_id,
                                                    &error)) {
              notifyError(error.empty() ? _("Failed to set command model.") : error);
              return;
            }
            notifyInfo(fmt::sprintf(_("Command model set to '%s / %s'."), item.provider_id.c_str(),
                                    item.model_id.c_str()));
          },
  });
}

void VinputEngine::handleAsrSelection(const PaletteItem& item) {
  auto core_config = LoadCoreConfig();
#if !VINPUT_ENABLE_LOCAL_ASR
  const auto* target_provider = CoreConfigView(core_config).AsrProvider(item.provider_id);
  if (target_provider && std::holds_alternative<LocalAsrProvider>(*target_provider)) {
    notifyError(fmt::sprintf(_("ASR provider '%s' is a local provider, which is not "
                               "supported in this Lite build."),
                             item.provider_id.c_str()));
    return;
  }
#endif
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
  notifyInfo(fmt::sprintf(_("ASR switch requested for '%s'."), item.text.c_str()));
}

void VinputEngine::registerAsrCommand() {
  palette_registry_.registerCommand(PaletteCommand{
      .id = "asr",
      .name = "asr",
      .alias = "a",
      .title = _("ASR"),
      .comment = _("ASR"),
      .description = _("Switch ASR provider or model"),
      .category = PaletteCategory::Asr,
      .getItems = [](VinputEngine* /*engine*/) { return LoadAsrItems(); },
      .onSelect = [this](VinputEngine* /*engine*/, const PaletteItem& item,
                         fcitx::InputContext* /*ic*/) { handleAsrSelection(item); },
  });
}

void VinputEngine::registerSceneCommand() {
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
                  .is_command_entry = false,
                  .command_token = {},
              });
            }
            return items;
          },
      .onSelect =
          [this](VinputEngine* /*engine*/, const PaletteItem& item, fcitx::InputContext* /*ic*/) {
            auto core_config = LoadCoreConfig();
            core_config.scenes.activeScene = item.scene_id;
            if (!SaveCoreConfig(core_config)) {
              notifyError(_("Failed to save scene config."));
              return;
            }
            active_scene_id_ = item.scene_id;
            notifyInfo(fmt::sprintf(_("Scene switched to '%s'."), item.text.c_str()));
          },
  });
}

bool IsClientActive(VinputEngine& engine, const PaletteItem& item) {
  std::vector<vinput::dbus::ClientStatus> statuses;
  if (engine.callGetClientStatuses(&statuses)) {
    const auto it = std::ranges::find_if(statuses, [&](const auto& s) { return s.id == item.id; });
    if (it != statuses.end()) {
      return it->running;
    }
  }
  return vinput::client::IsRunning(item.id);
}

void VinputEngine::handleClientToggle(const PaletteItem& item) {
  const bool is_running = IsClientActive(*this, item);
  std::string error;
  if (is_running) {
    if (!callStopClient(item.id, &error)) {
      notifyError(error.empty() ? _("Failed to stop client.") : error);
      return;
    }
    notifyInfo(fmt::sprintf(_("Client '%s' stopped."), item.id.c_str()));
    return;
  }

  if (!callStartClient(item.id, &error)) {
    notifyError(error.empty() ? _("Failed to start client.") : error);
    return;
  }
  notifyInfo(fmt::sprintf(_("Client '%s' started."), item.id.c_str()));
}

void VinputEngine::registerProcessCommand() {
  palette_registry_.registerCommand(PaletteCommand{
      .id = "proc",
      .name = "proc",
      .alias = "p",
      .title = _("Process"),
      .comment = _("Process"),
      .description = _("Manage client processes"),
      .category = PaletteCategory::Process,
      .getItems =
          [this](VinputEngine* /*engine*/) {
            std::vector<PaletteItem> items;
            const auto config = LoadCoreConfig();
            items.reserve(config.triggers.size() + config.receivers.size());

            std::unordered_map<std::string, vinput::dbus::ClientStatus> status_map;
            std::vector<vinput::dbus::ClientStatus> statuses;
            if (callGetClientStatuses(&statuses)) {
              for (auto& s : statuses) {
                status_map.emplace(s.id, std::move(s));
              }
            }

            const auto build_item = [&](const ClientSpec& client, bool is_trigger) {
              bool running = false;
              pid_t pid = -1;
              if (const auto it = status_map.find(client.id); it != status_map.end()) {
                running = it->second.running;
                pid = static_cast<pid_t>(it->second.pid);
              } else {
                pid = vinput::client::GetPid(client.id);
                running = (pid > 0);
              }
              return BuildClientPaletteItem(client, is_trigger, running, pid);
            };

            for (const auto& client : config.triggers) {
              items.push_back(build_item(client, true));
            }
            for (const auto& client : config.receivers) {
              items.push_back(build_item(client, false));
            }
            return items;
          },
      .onSelect = [this](VinputEngine* /*engine*/, const PaletteItem& item,
                         fcitx::InputContext* /*ic*/) { handleClientToggle(item); },
  });
}

void VinputEngine::initializePaletteRegistry() {
  palette_registry_.clear();
  registerModelCommand();
  registerAsrCommand();
  registerSceneCommand();
  registerProcessCommand();
}

bool VinputEngine::callStartClient(const std::string& client_id, std::string* error) {
  return callSyncRpc(vinput::dbus::kMethodStartClient, _("Failed to start client."), error,
                     &client_id);
}

bool VinputEngine::callStopClient(const std::string& client_id, std::string* error) {
  return callSyncRpc(vinput::dbus::kMethodStopClient, _("Failed to stop client."), error,
                     &client_id);
}

bool VinputEngine::callGetClientStatuses(std::vector<vinput::dbus::ClientStatus>* statuses) {
  if (statuses == nullptr || bus_ == nullptr || !daemonSyncAllowed()) {
    return false;
  }
  auto msg =
      bus_->createMethodCall(vinput::dbus::kBusName, vinput::dbus::kObjectPath,
                             vinput::dbus::kInterface, vinput::dbus::kMethodGetClientStatuses);
  auto reply = msg.call(vinput::runtime::kDbusCallTimeoutUsec);
  if (!reply || reply.isError()) {
    noteDaemonSyncFailure();
    return false;
  }
  clearDaemonSyncFailure();

  std::vector<fcitx::dbus::DBusStruct<std::string, bool, int64_t>> raw_list;
  reply >> raw_list;
  if (!reply) {
    return false;
  }

  statuses->clear();
  statuses->reserve(raw_list.size());
  for (const auto& item : raw_list) {
    statuses->push_back({
        .id = std::get<0>(item.data()),
        .running = std::get<1>(item.data()),
        .pid = std::get<2>(item.data()),
    });
  }
  return true;
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
    if (IsPaletteItemVisible(palette_items_[i], parsed, palette_query_)) {
      palette_filtered_indices_.push_back(i);
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

  if (!was_visible || ic == nullptr) {
    return;
  }

  const fcitx::Text empty;
  ic->inputPanel().setAuxUp(empty);
  ic->inputPanel().setCandidateList({});
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

bool VinputEngine::handlePaletteMenuKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (!palette_menu_visible_ || palette_menu_ic_ == nullptr) {
    return false;
  }

  // Pass menu toggle keys through to the dedicated hotkey handler so toggle-close works
  if (matchKeyListIndex(keyEvent.key(), menu_keys_, keyEvent.isRelease()) >= 0) {
    return false;
  }

  const bool printable = IsPrintableMenuInput(keyEvent.key());
  const bool handled = IsHandledPaletteMenuKey(keyEvent.key(), printable);
  if (keyEvent.isRelease()) {
    if (handled) {
      keyEvent.filterAndAccept();
      return true;
    }
    return false;
  }

  if (!handled) {
    hidePaletteMenu();
    return false;
  }

  if (IsPureModifierKey(keyEvent.key())) {
    keyEvent.filterAndAccept();
    return true;
  }

  if (HandlePaletteQueryEditing(keyEvent.key(), printable, &palette_query_)) {
    rebuildPaletteMenu(palette_menu_ic_);
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsEscapeKey(keyEvent.key()) || IsBackspaceKey(keyEvent.key())) {
    hidePaletteMenu();
    keyEvent.filterAndAccept();
    return true;
  }

  if (IsPagePrevKey(keyEvent.key()) || IsPageNextKey(keyEvent.key())) {
    const ParsedPaletteQuery parsed = ParsePaletteQuery(palette_query_, palette_registry_);
    ChangeCandidatePage(palette_menu_ic_, PaletteMenuTitle(parsed), IsPageNextKey(keyEvent.key()));
    keyEvent.filterAndAccept();
    return true;
  }

  auto candidate_list = palette_menu_ic_->inputPanel().candidateList();
  if (HandlePaletteCandidateAction(keyEvent.key(), candidate_list.get(), palette_menu_ic_)) {
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
    if (item.category == PaletteCategory::Process) {
      reloadPaletteItems();
    }
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
  if (ic == nullptr || payload.candidates.empty()) {
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

  for (std::size_t i = 0; i < result_candidates_.size(); ++i) {
    const auto& candidate = result_candidates_[i];
    candidate_list->append<ResultCandidateWord>(this, i, candidate.text,
                                                ResultCandidateComment(candidate));
  }

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

  if (!was_visible || ic == nullptr) {
    return;
  }

  const fcitx::Text empty;
  ic->inputPanel().setAuxUp(empty);
  ic->inputPanel().setCandidateList({});
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

bool VinputEngine::handleResultMenuKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (!result_menu_visible_ || result_menu_ic_ == nullptr) {
    return false;
  }

  const bool handled = IsHandledResultMenuKey(keyEvent.key(), page_prev_keys_, page_next_keys_);
  if (keyEvent.isRelease()) {
    if (handled) {
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

  auto candidate_list = result_menu_ic_->inputPanel().candidateList();
  if (candidate_list == nullptr || candidate_list->empty() || result_candidates_.empty()) {
    hideResultMenu();
    return false;
  }

  if (keyEvent.key().checkKeyList(page_prev_keys_) ||
      keyEvent.key().checkKeyList(page_next_keys_)) {
    ChangeCandidatePage(result_menu_ic_, ResultMenuTitle(result_candidates_.size()),
                        keyEvent.key().checkKeyList(page_next_keys_));
    keyEvent.filterAndAccept();
    return true;
  }

  if (HandleResultCandidateAction(keyEvent.key(), candidate_list.get(), result_menu_ic_)) {
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
