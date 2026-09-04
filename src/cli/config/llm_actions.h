#pragma once

#include <string>

#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

int RunLlmConfigList(Formatter& fmt, const CliContext& ctx);
int RunLlmConfigListAdapters(bool available, Formatter& fmt, const CliContext& ctx);
int RunLlmConfigAdd(const std::string& id, const std::string& baseUrl, const std::string& apiKey,
                    const std::string& extraBody, Formatter& fmt, const CliContext& ctx);
int RunLlmConfigInstallAdapter(const std::string& selector, Formatter& fmt, const CliContext& ctx);
int RunLlmConfigStartAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx);
int RunLlmConfigStopAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx);
int RunLlmConfigAutostartAdapter(const std::string& id, const std::string& state, bool enable,
                                 bool disable, Formatter& fmt, const CliContext& ctx);
int RunLlmConfigRemove(const std::string& id, Formatter& fmt, const CliContext& ctx);
int RunLlmConfigEdit(const std::string& id, const std::string& baseUrl, const std::string& apiKey,
                     const std::string& extraBody, bool hasBaseUrl, bool hasApiKey,
                     bool hasExtraBody, Formatter& fmt, const CliContext& ctx);
int RunLlmConfigTest(const std::string& id, Formatter& fmt, const CliContext& ctx);
