#pragma once
#include "cli/cli_context.h"
#include "cli/formatter.h"

int RunDaemonStart(Formatter& fmt, const CliContext& ctx);
int RunDaemonStop(Formatter& fmt, const CliContext& ctx);
int RunDaemonRestart(Formatter& fmt, const CliContext& ctx);
int RunDaemonLogs(bool follow, int lines, Formatter& fmt, const CliContext& ctx);
