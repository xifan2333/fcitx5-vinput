#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <unistd.h>

#include "common/i18n.h"

#include "cli/action.h"
#include "cli/config/register.h"
#include "cli/control/register.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

#include "config.h"

namespace {

#if VINPUT_ENABLE_LOCAL_ASR
constexpr char kCliVersion[] = VINPUT_VERSION;
#else
constexpr char kCliVersion[] = VINPUT_VERSION "-lite";
#endif

} // namespace

int main(int argc, char* argv[]) {
  vinput::i18n::Init();

  CLI::App app{_("vinput - Voice input manager")};
  app.require_subcommand(0, 1);
  app.set_help_flag("-h,--help", _("Print this help message and exit"));
  app.set_version_flag("-v,--version", kCliVersion,
                       _("Display program version information and exit"));

  bool json_output = false;
  app.add_flag("-j,--json", json_output, _("Output in JSON format"));

  vinput::cli::CliAction action;
  vinput::cli::config::RegisterConfigCli(app, &action);
  vinput::cli::control::RegisterControlCli(app, &action);

  CLI11_PARSE(app, argc, argv);

  if (argc == 1) {
    std::cout << app.help();
    return 0;
  }

  CliContext ctx;
  ctx.json_output = json_output;
  ctx.is_tty = (isatty(STDOUT_FILENO) == 1);

  auto fmt = CreateFormatter(ctx);
  if (action) {
    return action(*fmt, ctx);
  }
  return 0;
}
