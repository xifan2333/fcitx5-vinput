#include <CLI/CLI.hpp>
#include <memory>
#include <string>

#include "common/i18n.h"
#include "common/scene/postprocess_scene.h"

#include "cli/config/action.h"
#include "cli/config/register.h"
#include "cli/config/scene_actions.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli::config {

namespace {

struct SceneCliValues {
  std::string id;
  std::string label;
  std::string prompt;
  std::string providerId;
  std::string model;
  int llmMaxCandidates = vinput::scene::kDefaultLlmMaxCandidates;
  int timeoutMs = vinput::scene::kDefaultTimeoutMs;
  int contextLines = vinput::scene::kDefaultContextLines;
  bool rawCand = true;
  bool rawPrev = true;
};

struct SceneOptionBindings {
  CLI::Option* label = nullptr;
  CLI::Option* prompt = nullptr;
  CLI::Option* providerId = nullptr;
  CLI::Option* model = nullptr;
  CLI::Option* count = nullptr;
  CLI::Option* timeout = nullptr;
  CLI::Option* contextLines = nullptr;
  CLI::Option* rawCand = nullptr;
  CLI::Option* rawPrev = nullptr;
};

SceneOptionBindings BindSceneOptions(CLI::App* cmd, SceneCliValues* values, bool with_defaults) {
  SceneOptionBindings b;
  b.label = cmd->add_option("-l,--label", values->label, _("Display label"));
  b.prompt = cmd->add_option("-t,--prompt", values->prompt, _("LLM prompt"));
  b.providerId = cmd->add_option("-p,--provider", values->providerId, _("LLM provider id"));
  b.model = cmd->add_option("-m,--model", values->model, _("LLM model id"));
  b.count = cmd->add_option("-c,--count", values->llmMaxCandidates,
                            _("Maximum number of LLM candidates (0 to disable LLM)"));
  b.timeout = cmd->add_option("--timeout", values->timeoutMs, _("Request timeout in milliseconds"));
  b.contextLines = cmd->add_option("--context-lines", values->contextLines,
                                   _("Number of previous lines sent as LLM context"));
  b.rawCand = cmd->add_option("--raw-cand", values->rawCand,
                              _("Include raw ASR transcript in candidate options (true/false)"))
                  ->expected(0, 1)
                  ->default_str("true");
  b.rawPrev =
      cmd->add_option(
             "--raw-prev", values->rawPrev,
             _("Preview raw ASR transcript in floating panel during postprocessing (true/false)"))
          ->expected(0, 1)
          ->default_str("true");
  if (with_defaults) {
    b.count->default_val(vinput::scene::kDefaultLlmMaxCandidates);
    b.timeout->default_val(vinput::scene::kDefaultTimeoutMs);
    b.contextLines->default_val(vinput::scene::kDefaultContextLines);
  }
  return b;
}

} // namespace

void RegisterSceneCommands(CLI::App& app, CliAction* action) {
  auto* scene = app.add_subcommand("scene", _("Manage recognition scenes"));
  scene->require_subcommand(1);

  auto* list = scene->add_subcommand("list", _("List all scenes"));
  list->alias("ls");
  list->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) { return RunSceneConfigList(fmt, ctx); };
  });

  auto addState = std::make_shared<SceneCliValues>();
  auto* add = scene->add_subcommand("add", _("Add a new scene"));
  add->add_option("--id", addState->id, _("Scene id"))->required();
  BindSceneOptions(add, addState.get(), true);
  add->callback([action, addState]() {
    *action = [addState](Formatter& fmt, const CliContext& ctx) {
      return RunSceneConfigAdd(addState->id, addState->label, addState->prompt,
                               addState->providerId, addState->model, addState->llmMaxCandidates,
                               addState->timeoutMs, addState->contextLines, addState->rawCand,
                               addState->rawPrev, fmt, ctx);
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
    SceneCliValues values;
    bool hasLabel = false;
    bool hasPrompt = false;
    bool hasProvider = false;
    bool hasModel = false;
    bool hasLlmMaxCandidates = false;
    bool hasTimeout = false;
    bool hasContextLines = false;
    bool hasRawCand = false;
    bool hasRawPrev = false;
  };
  auto editState = std::make_shared<EditState>();
  auto* edit = scene->add_subcommand("edit", _("Edit a scene"));
  edit->alias("e");
  edit->add_option("id", editState->values.id, _("Scene id"))->required();
  const auto b = BindSceneOptions(edit, &editState->values, false);
  edit->callback([action, editState, b]() {
    editState->hasLabel = b.label->count() > 0;
    editState->hasPrompt = b.prompt->count() > 0;
    editState->hasProvider = b.providerId->count() > 0;
    editState->hasModel = b.model->count() > 0;
    editState->hasLlmMaxCandidates = b.count->count() > 0;
    editState->hasTimeout = b.timeout->count() > 0;
    editState->hasContextLines = b.contextLines->count() > 0;
    editState->hasRawCand = b.rawCand->count() > 0;
    editState->hasRawPrev = b.rawPrev->count() > 0;
    *action = [editState](Formatter& fmt, const CliContext& ctx) {
      const auto& v = editState->values;
      return RunSceneConfigEdit(v.id, v.label, v.prompt, v.providerId, v.model, v.llmMaxCandidates,
                                v.timeoutMs, v.contextLines, v.rawCand, v.rawPrev,
                                editState->hasLabel, editState->hasPrompt, editState->hasProvider,
                                editState->hasModel, editState->hasLlmMaxCandidates,
                                editState->hasTimeout, editState->hasContextLines,
                                editState->hasRawCand, editState->hasRawPrev, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::config
