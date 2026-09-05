#include "cli/config/scene_actions.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "common/config/core_config.h"
#include "common/i18n.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/string_utils.h"

#include "cli/utils/cli_helpers.h"

int RunSceneConfigList(Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();
  const auto& scenes = config.scenes.definitions;

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& scene : scenes) {
      bool active = (scene.id == config.scenes.activeScene);
      arr.push_back({{"id", scene.id},
                     {"label", vinput::scene::DisplayLabel(scene)},
                     {"prompt", scene.prompt},
                     {"provider_id", scene.provider_id},
                     {"model", scene.model},
                     {"count", scene.llm_max_candidates},
                     {"timeout_ms", scene.timeout_ms},
                     {"context_lines", scene.context_lines},
                     {"show_raw", scene.show_raw},
                     {"builtin", scene.builtin},
                     {"active", active}});
    }
    fmt.PrintJson(arr);
    return 0;
  }

  const std::vector<std::string> headers = {_("ID"),    _("LABEL"),    _("PROVIDER"), _("MODEL"),
                                            _("COUNT"), _("SHOW_RAW"), _("STATUS")};
  std::vector<std::vector<std::string>> rows;
  for (const auto& scene : scenes) {
    std::string label = vinput::scene::DisplayLabel(scene);
    std::string status = (scene.id == config.scenes.activeScene) ? "[*]" : "[ ]";
    std::string provider = scene.provider_id.empty() ? "-" : scene.provider_id;
    std::string model = scene.model.empty() ? "-" : scene.model;
    rows.push_back({scene.id, label, provider, model, std::to_string(scene.llm_max_candidates),
                    scene.show_raw ? _("yes") : _("no"), status});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunSceneConfigAdd(const std::string& id, const std::string& label, const std::string& prompt,
                      const std::string& provider_id, const std::string& model,
                      int llm_max_candidates, int timeout_ms, int context_lines, bool show_raw,
                      Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  vinput::scene::Definition def;
  def.id = vinput::str::TrimAsciiWhitespace(id);
  def.label = vinput::str::TrimAsciiWhitespace(label);
  def.prompt = vinput::str::TrimAsciiWhitespace(prompt);
  def.provider_id = vinput::str::TrimAsciiWhitespace(provider_id);
  def.model = vinput::str::TrimAsciiWhitespace(model);
  def.llm_max_candidates = llm_max_candidates;
  def.timeout_ms = timeout_ms;
  def.context_lines = context_lines;
  def.show_raw = show_raw;

  vinput::scene::Config scene_config = ToSceneConfig(config.scenes);
  std::string error;
  if (!vinput::scene::AddScene(&scene_config, def, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  FromSceneConfig(config.scenes, scene_config);

  if (!SaveConfigOrFail(config, fmt))
    return 1;

  fmt.PrintSuccess(vinput::str::FmtStr(_("Scene '%s' added."), id));
  return 0;
}

int RunSceneConfigUse(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  vinput::scene::Config scene_config = ToSceneConfig(config.scenes);
  std::string error;
  if (!vinput::scene::SetActiveScene(&scene_config, id, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  FromSceneConfig(config.scenes, scene_config);

  if (!SaveConfigOrFail(config, fmt))
    return 1;

  fmt.PrintSuccess(vinput::str::FmtStr(_("Default scene set to '%s'."), id));
  return 0;
}

int RunSceneConfigRemove(const std::string& id, bool force, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  vinput::scene::Config scene_config = ToSceneConfig(config.scenes);
  std::string error;
  if (!vinput::scene::RemoveScene(&scene_config, id, force, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  FromSceneConfig(config.scenes, scene_config);

  if (!SaveConfigOrFail(config, fmt))
    return 1;

  fmt.PrintSuccess(vinput::str::FmtStr(_("Scene '%s' removed."), id));
  return 0;
}

int RunSceneConfigEdit(const std::string& id, const std::string& label, const std::string& prompt,
                       const std::string& provider_id, const std::string& model,
                       int llm_max_candidates, int timeout_ms, int context_lines, bool show_raw,
                       bool hasLabel, bool hasPrompt, bool hasProvider, bool hasModel,
                       bool hasLlmMaxCandidates, bool hasTimeout, bool hasContextLines,
                       bool hasShowRaw, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  const vinput::scene::Definition* existing = nullptr;
  for (const auto& scene : config.scenes.definitions) {
    if (scene.id == id) {
      existing = &scene;
      break;
    }
  }
  if (!existing) {
    fmt.PrintError(vinput::str::FmtStr(_("Scene '%s' not found."), id));
    return 1;
  }

  vinput::scene::Definition updated = *existing;
  if (hasLabel) {
    updated.label = vinput::str::TrimAsciiWhitespace(label);
  }
  if (hasPrompt) {
    updated.prompt = vinput::str::TrimAsciiWhitespace(prompt);
  }
  if (hasProvider) {
    updated.provider_id = vinput::str::TrimAsciiWhitespace(provider_id);
  }
  if (hasModel) {
    updated.model = vinput::str::TrimAsciiWhitespace(model);
  }
  if (hasLlmMaxCandidates) {
    updated.llm_max_candidates = llm_max_candidates;
  }
  if (hasTimeout) {
    updated.timeout_ms = timeout_ms;
  }
  if (hasContextLines) {
    updated.context_lines = context_lines;
  }
  if (hasShowRaw) {
    updated.show_raw = show_raw;
  }

  vinput::scene::Config scene_config = ToSceneConfig(config.scenes);
  std::string error;
  if (!vinput::scene::UpdateScene(&scene_config, id, updated, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  FromSceneConfig(config.scenes, scene_config);

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }

  fmt.PrintSuccess(vinput::str::FmtStr(_("Scene '%s' updated."), id));
  return 0;
}
