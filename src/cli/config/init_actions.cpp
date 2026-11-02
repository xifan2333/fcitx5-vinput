#include "cli/config/init_actions.h"

#include <filesystem>
#include <fmt/printf.h>
#include <string>

#include "common/config/core_config.h"
#include "common/i18n.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace fs = std::filesystem;

int RunConfigInit(bool force, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  bool any_created = false;

  auto config_path = vinput::path::CoreConfigPath();
  if (fs::exists(config_path) && !force) {
    fmt.PrintInfo(::fmt::sprintf(_("Config already exists: %s"), config_path.string()));
  } else {
    std::string error;
    if (InitializeCoreConfig(&error)) {
      fmt.PrintSuccess(::fmt::sprintf(_("Created config: %s"), config_path.string()));
      any_created = true;
    } else {
      fmt.PrintError(error.empty() ? _("Failed to create config") : error);
      return 1;
    }
  }

  auto model_dir = vinput::path::DefaultModelBaseDir();
  if (fs::exists(model_dir)) {
    fmt.PrintInfo(::fmt::sprintf(_("Model dir already exists: %s"), model_dir.string()));
  } else {
    std::string error;
    if (!vinput::file::EnsureParentDirectory(model_dir / ".keep", &error)) {
      fmt.PrintError(::fmt::sprintf(_("Failed to create model dir: %s"), error));
      return 1;
    }
    fmt.PrintSuccess(::fmt::sprintf(_("Created model dir: %s"), model_dir.string()));
    any_created = true;
  }

  if (!any_created) {
    fmt.PrintInfo(_("Everything already initialized"));
  }

  return 0;
}
