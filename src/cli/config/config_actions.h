#pragma once

#include <string>

#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

int RunConfigDomainGet(const std::string& path, Formatter& fmt, const CliContext& ctx);
int RunConfigDomainSet(const std::string& path, const std::string& value, bool from_stdin,
                       Formatter& fmt, const CliContext& ctx);
int RunConfigDomainEdit(const std::string& target, Formatter& fmt, const CliContext& ctx);
int RunConfigMigrate(bool dry_run, Formatter& fmt, const CliContext& ctx);
