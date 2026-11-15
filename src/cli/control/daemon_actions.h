#pragma once

#include <string>

#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

int RunDaemonControlStatus(Formatter& fmt, const CliContext& ctx);
int RunDaemonControlStart(Formatter& fmt, const CliContext& ctx);
int RunDaemonControlStop(Formatter& fmt, const CliContext& ctx);
int RunDaemonControlRestart(Formatter& fmt, const CliContext& ctx);
int RunDaemonControlLogs(bool follow, int lines, const std::string& level, Formatter& fmt,
                         const CliContext& ctx);
int RunDaemonControlPipe(const std::string& scene_id, bool ime, Formatter& fmt,
                         const CliContext& ctx);
