#pragma once
#include "cli/cli_context.h"
#include "cli/formatter.h"
#include <string>

int RunSceneList(Formatter &fmt, const CliContext &ctx);
int RunSceneAdd(const std::string &id, const std::string &label,
                const std::string &prompt,
                Formatter &fmt, const CliContext &ctx);
int RunSceneUse(const std::string &id, Formatter &fmt, const CliContext &ctx);
int RunSceneRemove(const std::string &id, bool force, Formatter &fmt,
                   const CliContext &ctx);
