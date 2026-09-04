#include <CLI/CLI.hpp>
#include <memory>

#include "common/i18n.h"

#include "cli/config/action.h"
#include "cli/config/llm_actions.h"

namespace vinput::cli::config {

void RegisterLlmCommands(CLI::App& app, CliAction* action) {
  auto* llm = app.add_subcommand("llm", _("Manage LLM providers"));
  llm->require_subcommand(1);

  auto* list = llm->add_subcommand("list", _("List configured LLM providers"));
  list->alias("ls");
  list->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) { return RunLlmConfigList(fmt, ctx); };
  });

  auto id = std::make_shared<std::string>();
  auto baseUrl = std::make_shared<std::string>();
  auto apiKey = std::make_shared<std::string>();
  auto extraBody = std::make_shared<std::string>();
  auto* add = llm->add_subcommand("add", _("Add an LLM provider"));
  add->add_option("id", *id, _("Provider ID"))->required();
  add->add_option("-u,--base-url", *baseUrl, _("Base URL"))->required();
  add->add_option("-k,--api-key", *apiKey, _("API key"));
  add->add_option("-e,--extra-body", *extraBody,
                  _("Extra JSON object merged into each request body (e.g. "
                    "'{\"enable_thinking\": false}')"));
  add->callback([action, id, baseUrl, apiKey, extraBody]() {
    *action = [id, baseUrl, apiKey, extraBody](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigAdd(*id, *baseUrl, *apiKey, *extraBody, fmt, ctx);
    };
  });

  auto removeId = std::make_shared<std::string>();
  auto* remove = llm->add_subcommand("remove", _("Remove an LLM provider"));
  remove->alias("rm");
  remove->add_option("id", *removeId, _("Provider ID"))->required();
  remove->callback([action, removeId]() {
    *action = [removeId](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigRemove(*removeId, fmt, ctx);
    };
  });

  struct EditState {
    std::string id;
    std::string baseUrl;
    std::string apiKey;
    std::string extraBody;
    bool hasBaseUrl = false;
    bool hasApiKey = false;
    bool hasExtraBody = false;
  };
  auto editState = std::make_shared<EditState>();
  auto* edit = llm->add_subcommand("edit", _("Edit an LLM provider"));
  edit->alias("e");
  edit->add_option("id", editState->id, _("Provider ID"))->required();
  auto* editUrlOpt = edit->add_option("-u,--base-url", editState->baseUrl, _("Base URL"));
  auto* editKeyOpt = edit->add_option("-k,--api-key", editState->apiKey, _("API key"));
  auto* editExtraOpt =
      edit->add_option("-e,--extra-body", editState->extraBody,
                       _("Extra JSON object merged into each request body; pass '{}' to clear"));
  edit->callback([action, editState, editUrlOpt, editKeyOpt, editExtraOpt]() {
    editState->hasBaseUrl = editUrlOpt->count() > 0;
    editState->hasApiKey = editKeyOpt->count() > 0;
    editState->hasExtraBody = editExtraOpt->count() > 0;
    *action = [editState](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigEdit(editState->id, editState->baseUrl, editState->apiKey,
                              editState->extraBody, editState->hasBaseUrl, editState->hasApiKey,
                              editState->hasExtraBody, fmt, ctx);
    };
  });

  auto testId = std::make_shared<std::string>();
  auto* test = llm->add_subcommand("test", _("Test LLM provider connectivity"));
  test->add_option("id", *testId, _("Provider ID"))->required();
  test->callback([action, testId]() {
    *action = [testId](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigTest(*testId, fmt, ctx);
    };
  });
}

void RegisterAdapterCommands(CLI::App& app, CliAction* action) {
  auto* adapter = app.add_subcommand("adapter", _("Manage LLM adapters"));
  adapter->require_subcommand(1);

  auto available = std::make_shared<bool>(false);
  auto* list = adapter->add_subcommand("list", _("List adapters"));
  list->alias("ls");
  list->add_flag("-a,--available", *available, _("List remote adapters"));
  list->callback([action, available]() {
    *action = [available](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigListAdapters(*available, fmt, ctx);
    };
  });

  auto selector = std::make_shared<std::string>();
  auto* add = adapter->add_subcommand("add", _("Add an adapter"));
  add->add_option("id", *selector, _("Adapter short ID"))->required();
  add->callback([action, selector]() {
    *action = [selector](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigInstallAdapter(*selector, fmt, ctx);
    };
  });

  auto startId = std::make_shared<std::string>();
  auto* start = adapter->add_subcommand("start", _("Start an adapter"));
  start->add_option("id", *startId, _("Adapter short ID"))->required();
  start->callback([action, startId]() {
    *action = [startId](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigStartAdapter(*startId, fmt, ctx);
    };
  });

  auto stopId = std::make_shared<std::string>();
  auto* stop = adapter->add_subcommand("stop", _("Stop an adapter"));
  stop->add_option("id", *stopId, _("Adapter short ID"))->required();
  stop->callback([action, stopId]() {
    *action = [stopId](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigStopAdapter(*stopId, fmt, ctx);
    };
  });

  auto restartId = std::make_shared<std::string>();
  auto* restart = adapter->add_subcommand("restart", _("Restart an adapter"));
  restart->add_option("id", *restartId, _("Adapter short ID"))->required();
  restart->callback([action, restartId]() {
    *action = [restartId](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigRestartAdapter(*restartId, fmt, ctx);
    };
  });

  auto enableId = std::make_shared<std::string>();
  auto* enable = adapter->add_subcommand("enable", _("Enable adapter autostart with daemon"));
  enable->add_option("id", *enableId, _("Adapter short ID"))->required();
  enable->callback([action, enableId]() {
    *action = [enableId](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigEnableAdapter(*enableId, fmt, ctx);
    };
  });

  auto disableId = std::make_shared<std::string>();
  auto* disable = adapter->add_subcommand("disable", _("Disable adapter autostart with daemon"));
  disable->add_option("id", *disableId, _("Adapter short ID"))->required();
  disable->callback([action, disableId]() {
    *action = [disableId](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigDisableAdapter(*disableId, fmt, ctx);
    };
  });

  auto autostartId = std::make_shared<std::string>();
  auto autostartState = std::make_shared<std::string>();
  auto autostartEnable = std::make_shared<bool>(false);
  auto autostartDisable = std::make_shared<bool>(false);
  auto* autostart =
      adapter->add_subcommand("autostart", _("Configure or show adapter autostart with daemon"));
  autostart->add_option("id", *autostartId, _("Adapter short ID"))->required();
  autostart->add_option("state", *autostartState,
                        _("Target autostart state (on/off, enable/disable)"));
  autostart->add_flag("-e,--enable", *autostartEnable, _("Enable autostart with daemon"));
  autostart->add_flag("-d,--disable", *autostartDisable, _("Disable autostart with daemon"));
  autostart->callback([action, autostartId, autostartState, autostartEnable, autostartDisable]() {
    *action = [autostartId, autostartState, autostartEnable,
               autostartDisable](Formatter& fmt, const CliContext& ctx) {
      return RunLlmConfigAutostartAdapter(*autostartId, *autostartState, *autostartEnable,
                                          *autostartDisable, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::config
