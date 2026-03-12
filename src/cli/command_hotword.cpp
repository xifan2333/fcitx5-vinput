#include "cli/command_hotword.h"
#include "cli/editor_utils.h"
#include "common/i18n.h"
#include "common/core_config.h"

int RunHotwordList(Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  if (ctx.json_output) {
    nlohmann::json obj;
    obj["hotwords_file"] = config.hotwordsFile;
    fmt.PrintJson(obj);
    return 0;
  }

  if (config.hotwordsFile.empty()) {
    fmt.PrintInfo(_("No hotwords file configured."));
  } else {
    fmt.PrintInfo(config.hotwordsFile.c_str());
  }
  return 0;
}

int RunHotwordLoad(const std::string &file_path, Formatter &fmt,
                   const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  config.hotwordsFile = file_path;
  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }
  fmt.PrintSuccess(_("Hotwords file path saved."));
  return 0;
}

int RunHotwordClear(Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  config.hotwordsFile.clear();
  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }
  fmt.PrintSuccess(_("Hotwords file path cleared."));
  return 0;
}

int RunHotwordEdit(Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  if (config.hotwordsFile.empty()) {
    fmt.PrintError(_("No hotwords file configured. Use 'hotword set <path>' first."));
    return 1;
  }

  int ret = OpenInEditor(config.hotwordsFile);
  if (ret != 0) {
    fmt.PrintError(_("Editor exited with error."));
    return ret;
  }

  fmt.PrintSuccess(_("Hotwords file updated."));
  return 0;
}
