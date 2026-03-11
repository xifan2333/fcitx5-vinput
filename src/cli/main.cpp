#include <CLI/CLI.hpp>
#include <string>
#include <unistd.h>

#include "cli/cli_context.h"
#include "cli/command_config.h"
#include "cli/command_daemon.h"
#include "cli/command_device.h"
#include "cli/command_hotword.h"
#include "cli/command_init.h"
#include "cli/command_llm.h"
#include "cli/command_model.h"
#include "cli/command_scene.h"
#include "cli/command_status.h"
#include "cli/formatter.h"
#include "common/i18n.h"
#include "common/vinput_config.h"

int main(int argc, char *argv[]) {
  CLI::App app{"vinput - Voice input model and daemon manager"};
  app.require_subcommand(0, 1);

  bool json_output = false;
  app.add_flag("--json", json_output, "Output in JSON format");

  // ---- model subcommand ----
  auto *model_cmd = app.add_subcommand("model", "Manage ASR models");
  model_cmd->require_subcommand(1);

  bool model_list_remote = false;
  auto *model_list = model_cmd->add_subcommand(
      "list", "List installed (and optionally remote) models");
  model_list->add_flag("--remote", model_list_remote,
                       "Include remote models from registry");

  std::string model_add_name;
  auto *model_add =
      model_cmd->add_subcommand("add", "Download and install a model");
  model_add->add_option("name", model_add_name, "Model name")->required();

  std::string model_use_name;
  auto *model_use = model_cmd->add_subcommand("use", "Set active model");
  model_use->add_option("name", model_use_name, "Model name")->required();

  std::string model_remove_name;
  bool model_remove_force = false;
  auto *model_remove =
      model_cmd->add_subcommand("remove", "Remove an installed model");
  model_remove->add_option("name", model_remove_name, "Model name")->required();
  model_remove->add_flag("--force", model_remove_force, "Skip confirmation");

  std::string model_info_name;
  auto *model_info = model_cmd->add_subcommand("info", "Show model details");
  model_info->add_option("name", model_info_name, "Model name")->required();

  // ---- scene subcommand ----
  auto *scene_cmd = app.add_subcommand("scene", "Manage recognition scenes");
  scene_cmd->require_subcommand(1);

  auto *scene_list = scene_cmd->add_subcommand("list", "List all scenes");

  auto *scene_add = scene_cmd->add_subcommand("add", "Add a new scene");
  std::string scene_add_id;
  std::string scene_add_label;
  std::string scene_add_prompt;
  scene_add->add_option("--id", scene_add_id, "Scene ID")->required();
  scene_add->add_option("--label", scene_add_label, "Display label");
  scene_add->add_option("--prompt", scene_add_prompt, "LLM prompt");

  std::string scene_use_id;
  auto *scene_use = scene_cmd->add_subcommand("use", "Set active scene");
  scene_use->add_option("id", scene_use_id, "Scene ID")->required();

  std::string scene_remove_id;
  bool scene_remove_force = false;
  auto *scene_remove = scene_cmd->add_subcommand("remove", "Remove a scene");
  scene_remove->add_option("id", scene_remove_id, "Scene ID")->required();
  scene_remove->add_flag("--force", scene_remove_force,
                         "Force removal of built-in scenes");

  // ---- llm subcommand ----
  auto *llm_cmd = app.add_subcommand("llm", "Manage LLM providers");
  llm_cmd->require_subcommand(1);

  auto *llm_list =
      llm_cmd->add_subcommand("list", "List configured LLM providers");

  std::string llm_add_name;
  std::string llm_add_base_url;
  std::string llm_add_model;
  std::string llm_add_api_key;
  auto *llm_add = llm_cmd->add_subcommand("add", "Add an LLM provider");
  llm_add->add_option("name", llm_add_name, "Provider name")->required();
  llm_add->add_option("--base-url", llm_add_base_url, "Base URL")->required();
  llm_add->add_option("--model", llm_add_model, "Model name")->required();
  llm_add->add_option("--api-key", llm_add_api_key, "API key");

  std::string llm_use_name;
  auto *llm_use = llm_cmd->add_subcommand("use", "Set active LLM provider");
  llm_use->add_option("name", llm_use_name, "Provider name")->required();

  std::string llm_remove_name;
  bool llm_remove_force = false;
  auto *llm_remove =
      llm_cmd->add_subcommand("remove", "Remove an LLM provider");
  llm_remove->add_option("name", llm_remove_name, "Provider name")->required();
  llm_remove->add_flag("--force", llm_remove_force, "Skip confirmation");

  auto *llm_enable =
      llm_cmd->add_subcommand("enable", "Enable LLM processing globally");
  auto *llm_disable =
      llm_cmd->add_subcommand("disable", "Disable LLM processing globally");

  // ---- config subcommand ----
  auto *config_cmd =
      app.add_subcommand("config", "Read or write configuration values");
  config_cmd->require_subcommand(1);

  std::string config_get_path;
  auto *config_get =
      config_cmd->add_subcommand("get", "Get a config value by dotpath");
  config_get
      ->add_option("path", config_get_path,
                   "Config dotpath (e.g. fcitx.triggerKey)")
      ->required();

  std::string config_set_path;
  std::string config_set_value;
  bool config_set_stdin = false;
  auto *config_set =
      config_cmd->add_subcommand("set", "Set a config value by dotpath");
  config_set->add_option("path", config_set_path, "Config dotpath")->required();
  config_set->add_option("value", config_set_value, "New value");
  config_set->add_flag("--stdin", config_set_stdin, "Read value from stdin");

  std::string config_edit_target;
  auto *config_edit =
      config_cmd->add_subcommand("edit", "Open config file in editor");
  config_edit
      ->add_option("target", config_edit_target,
                   "Config target: fcitx or extra")
      ->required();

  // ---- daemon subcommand ----
  auto *daemon_cmd = app.add_subcommand("daemon", "Control the vinput daemon");
  daemon_cmd->require_subcommand(1);

  auto *daemon_start = daemon_cmd->add_subcommand("start", "Start the daemon");
  auto *daemon_stop = daemon_cmd->add_subcommand("stop", "Stop the daemon");
  auto *daemon_restart =
      daemon_cmd->add_subcommand("restart", "Restart the daemon");

  bool daemon_logs_follow = false;
  int daemon_logs_lines = 20;
  auto *daemon_logs = daemon_cmd->add_subcommand("logs", "Show daemon logs");
  daemon_logs->add_flag("-f,--follow", daemon_logs_follow, "Follow log output");
  daemon_logs->add_option("-n", daemon_logs_lines, "Number of lines to show")
      ->default_val(20);

  // ---- init subcommand ----
  auto *init_cmd =
      app.add_subcommand("init", "Initialize default config and directories");
  bool init_force = false;
  init_cmd->add_flag("--force", init_force, "Overwrite existing config");

  // ---- hotword subcommand ----
  auto *hotword_cmd = app.add_subcommand("hotword", "Manage hotwords");
  hotword_cmd->require_subcommand(1);

  auto *hotword_list =
      hotword_cmd->add_subcommand("list", "List configured hotwords");

  std::string hotword_load_file;
  auto *hotword_load = hotword_cmd->add_subcommand(
      "load", "Load hotwords from file (or - for stdin)");
  hotword_load->add_option("file", hotword_load_file, "Path to text file")
      ->required();

  auto *hotword_clear =
      hotword_cmd->add_subcommand("clear", "Remove all hotwords");
  auto *hotword_edit =
      hotword_cmd->add_subcommand("edit", "Edit hotwords in editor");

  // ---- device subcommand ----
  auto *device_cmd = app.add_subcommand("device", "Manage capture devices");
  device_cmd->require_subcommand(1);

  auto *device_list =
      device_cmd->add_subcommand("list", "List available audio input devices");

  std::string device_use_name;
  auto *device_use =
      device_cmd->add_subcommand("use", "Set active capture device");
  device_use->add_option("name", device_use_name, "Device name or 'default'")
      ->required();

  // ---- status subcommand ----
  auto *status_cmd = app.add_subcommand("status", "Show overall vinput status");

  // ---- Parse ----
  CLI11_PARSE(app, argc, argv);

  // Build context after parsing
  CliContext ctx;
  ctx.json_output = json_output;
  vinput::i18n::Init();
  ctx.is_tty = (isatty(STDOUT_FILENO) == 1);

  auto fmt = CreateFormatter(ctx);

  // ---- Dispatch ----

  // model
  if (model_list->parsed()) {
    return RunModelList(model_list_remote, *fmt, ctx);
  } else if (model_add->parsed()) {
    return RunModelAdd(model_add_name, *fmt, ctx);
  } else if (model_use->parsed()) {
    return RunModelUse(model_use_name, *fmt, ctx);
  } else if (model_remove->parsed()) {
    return RunModelRemove(model_remove_name, model_remove_force, *fmt, ctx);
  } else if (model_info->parsed()) {
    return RunModelInfo(model_info_name, *fmt, ctx);
  }

  // scene
  else if (scene_list->parsed()) {
    return RunSceneList(*fmt, ctx);
  } else if (scene_add->parsed()) {
    return RunSceneAdd(scene_add_id, scene_add_label, scene_add_prompt,
                       *fmt, ctx);
  } else if (scene_use->parsed()) {
    return RunSceneUse(scene_use_id, *fmt, ctx);
  } else if (scene_remove->parsed()) {
    return RunSceneRemove(scene_remove_id, scene_remove_force, *fmt, ctx);
  }

  // llm
  else if (llm_list->parsed()) {
    return RunLlmList(*fmt, ctx);
  } else if (llm_add->parsed()) {
    return RunLlmAdd(llm_add_name, llm_add_base_url, llm_add_model,
                     llm_add_api_key, *fmt, ctx);
  } else if (llm_use->parsed()) {
    return RunLlmUse(llm_use_name, *fmt, ctx);
  } else if (llm_remove->parsed()) {
    return RunLlmRemove(llm_remove_name, llm_remove_force, *fmt, ctx);
  } else if (llm_enable->parsed()) {
    return RunLlmEnable(*fmt, ctx);
  } else if (llm_disable->parsed()) {
    return RunLlmDisable(*fmt, ctx);
  }

  // hotword
  else if (hotword_list->parsed()) {
    return RunHotwordList(*fmt, ctx);
  } else if (hotword_load->parsed()) {
    return RunHotwordLoad(hotword_load_file, *fmt, ctx);
  } else if (hotword_clear->parsed()) {
    return RunHotwordClear(*fmt, ctx);
  } else if (hotword_edit->parsed()) {
    return RunHotwordEdit(*fmt, ctx);
  }

  // device
  else if (device_list->parsed()) {
    return RunDeviceList(*fmt, ctx);
  } else if (device_use->parsed()) {
    return RunDeviceUse(device_use_name, *fmt, ctx);
  }

  // config
  else if (config_get->parsed()) {
    return RunConfigGet(config_get_path, *fmt, ctx);
  } else if (config_set->parsed()) {
    return RunConfigSet(config_set_path, config_set_value, config_set_stdin,
                        *fmt, ctx);
  } else if (config_edit->parsed()) {
    return RunConfigEdit(config_edit_target, *fmt, ctx);
  }

  // daemon
  else if (daemon_start->parsed()) {
    return RunDaemonStart(*fmt, ctx);
  } else if (daemon_stop->parsed()) {
    return RunDaemonStop(*fmt, ctx);
  } else if (daemon_restart->parsed()) {
    return RunDaemonRestart(*fmt, ctx);
  } else if (daemon_logs->parsed()) {
    return RunDaemonLogs(daemon_logs_follow, daemon_logs_lines, *fmt, ctx);
  }

  // status
  else if (status_cmd->parsed()) {
    return RunStatus(*fmt, ctx);
  }

  // init
  else if (init_cmd->parsed()) {
    return RunInit(init_force, *fmt, ctx);
  }

  return 0;
}
