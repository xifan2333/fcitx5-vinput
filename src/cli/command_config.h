#pragma once
#include "cli/cli_context.h"
#include "cli/formatter.h"
#include <string>

int RunConfigGet(const std::string& path, Formatter& fmt, const CliContext& ctx);
int RunConfigSet(const std::string& path, const std::string& value, bool from_stdin, Formatter& fmt, const CliContext& ctx);
int RunConfigEdit(const std::string& target, Formatter& fmt, const CliContext& ctx);
