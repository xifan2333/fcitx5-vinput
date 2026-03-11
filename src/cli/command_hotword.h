#pragma once

#include "cli/cli_context.h"
#include "cli/formatter.h"
#include <string>

int RunHotwordList(Formatter &fmt, const CliContext &ctx);
int RunHotwordLoad(const std::string &file_path, Formatter &fmt,
                   const CliContext &ctx);
int RunHotwordClear(Formatter &fmt, const CliContext &ctx);
int RunHotwordEdit(Formatter &fmt, const CliContext &ctx);
