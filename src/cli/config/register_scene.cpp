#include <CLI/CLI.hpp>
#include <memory>

#include "common/i18n.h"
#include "common/scene/postprocess_scene.h"

#include "cli/config/action.h"
#include "cli/config/scene_actions.h"

namespace vinput::cli::config {

void RegisterSceneCommands(CLI::App& app, CliAction* action) {
  auto* scene = app.add_subcommand("scene", _("Manage recognition scenes"));
  scene->require_subcommand(1);

  auto* list = scene->add_subcommand("list", _("List all scenes"));
  list->alias("ls");
  list->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) { return RunSceneConfigList(fmt, ctx); };
  });

  struct AddState {
    std::string id;
    std::string label;
    std::string prompt;
    std::string providerId;
    std::string model;
    int llmMaxCandidates = vinput::scene::kDefaultLlmMaxCandidates;
    int timeoutMs = vinput::scene::kDefaultTimeoutMs;
    int contextLines = vinput::scene::kDefaultContextLines;
    bool showRaw = true;
  };
  auto addState = std::make_shared<AddState>();
  auto* add = scene->add_subcommand("add", _("Add a new scene"));
  add->add_option("--id", addState->id, _("Scene id"))->required();
  add->add_option("-l,--label", addState->label, _("Display label"));
  add->add_option("-t,--prompt", addState->prompt, _("LLM prompt"));
  add->add_option("-p,--provider", addState->providerId, _("LLM provider id"));
  add->add_option("-m,--model", addState->model, _("LLM model id"));
  add->add_option("-c,--count", addState->llmMaxCandidates,
                  _("Maximum number of LLM candidates (0 to disable LLM)"))
      ->default_val(vinput::scene::kDefaultLlmMaxCandidates);
  add->add_option("--timeout", addState->timeoutMs, _("Request timeout in milliseconds"))
      ->default_val(vinput::scene::kDefaultTimeoutMs);
  add->add_option("--context-lines", addState->contextLines,
                  _("Number of previous lines sent as LLM context"))
      ->default_val(vinput::scene::kDefaultContextLines);
  add->add_option("--show-raw", addState->showRaw,
                  _("Include raw ASR transcript in candidate options (true/false)"))
      ->expected(0, 1)
      ->default_str("true");
  add->callback([action, addState]() {
    *action = [addState](Formatter& fmt, const CliContext& ctx) {
      return RunSceneConfigAdd(addState->id, addState->label, addState->prompt,
                               addState->providerId, addState->model, addState->llmMaxCandidates,
                               addState->timeoutMs, addState->contextLines, addState->showRaw, fmt,
                               ctx);
    };
  });

  auto useId = std::make_shared<std::string>();
  auto* use = scene->add_subcommand("use", _("Set active scene"));
  use->add_option("id", *useId, _("Scene id"))->required();
  use->callback([action, useId]() {
    *action = [useId](Formatter& fmt, const CliContext& ctx) {
      return RunSceneConfigUse(*useId, fmt, ctx);
    };
  });

  auto removeId = std::make_shared<std::string>();
  auto* remove = scene->add_subcommand("remove", _("Remove a scene"));
  remove->alias("rm");
  remove->add_option("id", *removeId, _("Scene id"))->required();
  remove->callback([action, removeId]() {
    *action = [removeId](Formatter& fmt, const CliContext& ctx) {
      return RunSceneConfigRemove(*removeId, false, fmt, ctx);
    };
  });

  struct EditState {
    std::string id;
    std::string label;
    std::string prompt;
    std::string providerId;
    std::string model;
    int llmMaxCandidates = vinput::scene::kDefaultLlmMaxCandidates;
    int timeoutMs = vinput::scene::kDefaultTimeoutMs;
    int contextLines = vinput::scene::kDefaultContextLines;
    bool showRaw = true;
    bool hasLabel = false;
    bool hasPrompt = false;
    bool hasProvider = false;
    bool hasModel = false;
    bool hasLlmMaxCandidates = false;
    bool hasTimeout = false;
    bool hasContextLines = false;
    bool hasShowRaw = false;
  };
  auto editState = std::make_shared<EditState>();
  auto* edit = scene->add_subcommand("edit", _("Edit a scene"));
  edit->alias("e");
  edit->add_option("id", editState->id, _("Scene id"))->required();
  auto* eLbl = edit->add_option("-l,--label", editState->label, _("Display label"));
  auto* ePmt = edit->add_option("-t,--prompt", editState->prompt, _("LLM prompt"));
  auto* ePrv = edit->add_option("-p,--provider", editState->providerId, _("LLM provider id"));
  auto* eMdl = edit->add_option("-m,--model", editState->model, _("LLM model id"));
  auto* eCnd = edit->add_option("-c,--count", editState->llmMaxCandidates,
                                _("Maximum number of LLM candidates (0 to disable LLM)"));
  auto* eTmo =
      edit->add_option("--timeout", editState->timeoutMs, _("Request timeout in milliseconds"));
  auto* eCtx = edit->add_option("--context-lines", editState->contextLines,
                                _("Number of previous lines sent as LLM context"));
  auto* eRaw = edit->add_option("--show-raw", editState->showRaw,
                                _("Include raw ASR transcript in candidate options (true/false)"))
                   ->expected(0, 1)
                   ->default_str("true");
  edit->callback([action, editState, eLbl, ePmt, ePrv, eMdl, eCnd, eTmo, eCtx, eRaw]() {
    editState->hasLabel = eLbl->count() > 0;
    editState->hasPrompt = ePmt->count() > 0;
    editState->hasProvider = ePrv->count() > 0;
    editState->hasModel = eMdl->count() > 0;
    editState->hasLlmMaxCandidates = eCnd->count() > 0;
    editState->hasTimeout = eTmo->count() > 0;
    editState->hasContextLines = eCtx->count() > 0;
    editState->hasShowRaw = eRaw->count() > 0;
    *action = [editState](Formatter& fmt, const CliContext& ctx) {
      return RunSceneConfigEdit(
          editState->id, editState->label, editState->prompt, editState->providerId,
          editState->model, editState->llmMaxCandidates, editState->timeoutMs,
          editState->contextLines, editState->showRaw, editState->hasLabel, editState->hasPrompt,
          editState->hasProvider, editState->hasModel, editState->hasLlmMaxCandidates,
          editState->hasTimeout, editState->hasContextLines, editState->hasShowRaw, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::config
