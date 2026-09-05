#include <CLI/CLI.hpp>
#include <memory>
#include <string>

#include "common/i18n.h"

#include "cli/config/action.h"
#include "cli/config/asr_actions.h"

#include "config.h"

namespace vinput::cli::config {

#if VINPUT_ENABLE_LOCAL_ASR
void RegisterHotwordCommands(CLI::App& app, CliAction* action) {
  auto* hotword = app.add_subcommand("hotword", _("Manage hotword file"));
  hotword->require_subcommand(1);

  auto* get = hotword->add_subcommand("get", _("Show configured hotword file path"));
  get->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigGetHotword(fmt, ctx);
    };
  });

  auto path = std::make_shared<std::string>();
  auto* set = hotword->add_subcommand("set", _("Set hotword file path"));
  set->add_option("path", *path, _("Path to hotword file"))->required();
  set->callback([action, path]() {
    *action = [path](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigSetHotword(*path, fmt, ctx);
    };
  });

  auto* clear = hotword->add_subcommand("clear", _("Clear hotword file path"));
  clear->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigClearHotword(fmt, ctx);
    };
  });

  auto* edit = hotword->add_subcommand("edit", _("Edit hotword file"));
  edit->alias("e");
  edit->callback([action]() {
    *action = [](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigEditHotword(fmt, ctx);
    };
  });
}
#endif

#if VINPUT_ENABLE_LOCAL_ASR
void RegisterModelCommands(CLI::App& app, CliAction* action) {
  auto* model = app.add_subcommand("model", _("Manage local ASR models"));
  model->require_subcommand(1);

  auto available = std::make_shared<bool>(false);
  auto* list = model->add_subcommand("list", _("List models"));
  list->alias("ls");
  list->add_flag("-a,--available", *available, _("List remote models"));
  list->callback([action, available]() {
    *action = [available](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigListModels(*available, fmt, ctx);
    };
  });

  auto selector = std::make_shared<std::string>();
  auto* add = model->add_subcommand("add", _("Add a model"));
  add->add_option("id", *selector, _("Model short ID"))->required();
  add->callback([action, selector]() {
    *action = [selector](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigInstallModel(*selector, fmt, ctx);
    };
  });

  auto removeSelector = std::make_shared<std::string>();
  auto* remove = model->add_subcommand("remove", _("Remove a model"));
  remove->alias("rm");
  remove->add_option("id", *removeSelector, _("Model short ID"))->required();
  remove->callback([action, removeSelector]() {
    *action = [removeSelector](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigRemoveModel(*removeSelector, fmt, ctx);
    };
  });

  auto useSelector = std::make_shared<std::string>();
  auto* use = model->add_subcommand("use", _("Set active model"));
  use->add_option("id", *useSelector, _("Model short ID"))->required();
  use->callback([action, useSelector]() {
    *action = [useSelector](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigUseModel(*useSelector, fmt, ctx);
    };
  });

  auto infoSelector = std::make_shared<std::string>();
  auto* info = model->add_subcommand("info", _("Show model details"));
  info->add_option("id", *infoSelector, _("Model short ID"))->required();
  info->callback([action, infoSelector]() {
    *action = [infoSelector](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigModelInfo(*infoSelector, fmt, ctx);
    };
  });
}
#endif

void RegisterProviderCommands(CLI::App& app, CliAction* action) {
  auto* provider = app.add_subcommand("provider", _("Manage ASR providers"));
  provider->require_subcommand(1);

  auto available = std::make_shared<bool>(false);
  auto* list = provider->add_subcommand("list", _("List providers"));
  list->alias("ls");
  list->add_flag("-a,--available", *available, _("List remote providers"));
  list->callback([action, available]() {
    *action = [available](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigListProviders(*available, fmt, ctx);
    };
  });

  auto addSelector = std::make_shared<std::string>();
  auto* add = provider->add_subcommand("add", _("Add a provider"));
  add->add_option("id", *addSelector, _("Provider short ID"))->required();
  add->callback([action, addSelector]() {
    *action = [addSelector](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigInstallProvider(*addSelector, fmt, ctx);
    };
  });

  auto useId = std::make_shared<std::string>();
  auto* use = provider->add_subcommand("use", _("Set active provider"));
  use->add_option("id", *useId, _("Provider short ID"))->required();
  use->callback([action, useId]() {
    *action = [useId](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigUse(*useId, fmt, ctx);
    };
  });

  auto editId = std::make_shared<std::string>();
  auto* edit = provider->add_subcommand("edit", _("Edit provider script"));
  edit->alias("e");
  edit->add_option("id", *editId, _("Provider short ID"))->required();
  edit->callback([action, editId]() {
    *action = [editId](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigEdit(*editId, fmt, ctx);
    };
  });

  auto removeId = std::make_shared<std::string>();
  auto* remove = provider->add_subcommand("remove", _("Remove a provider"));
  remove->alias("rm");
  remove->add_option("id", *removeId, _("Provider short ID"))->required();
  remove->callback([action, removeId]() {
    *action = [removeId](Formatter& fmt, const CliContext& ctx) {
      return RunAsrConfigRemove(*removeId, fmt, ctx);
    };
  });
}

} // namespace vinput::cli::config
