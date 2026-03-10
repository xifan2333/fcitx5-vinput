#pragma once
#include "cli/cli_context.h"
#include "cli/formatter.h"
#include <string>

int RunModelList(bool remote, Formatter& fmt, const CliContext& ctx);
int RunModelAdd(const std::string& name, Formatter& fmt, const CliContext& ctx);
int RunModelEdit(const std::string& name, Formatter& fmt, const CliContext& ctx);
int RunModelUse(const std::string& name, Formatter& fmt, const CliContext& ctx);
int RunModelRemove(const std::string& name, bool force, Formatter& fmt, const CliContext& ctx);
int RunModelInfo(const std::string& name, Formatter& fmt, const CliContext& ctx);
