#include "cli/command_config.h"

#include <cstdio>
#include <iostream>
#include <string>

#include "cli/editor_utils.h"
#include "cli/i18n.h"
#include "common/config_path.h"

int RunConfigGet(const std::string& path, Formatter& fmt, const CliContext& ctx) {
    std::string value, error;
    if (!vinput::config::GetConfigValue(path, &value, &error)) {
        fmt.PrintError(error);
        return 1;
    }
    if (ctx.json_output) {
        fmt.PrintJson({{"path", path}, {"value", value}});
    } else {
        std::puts(value.c_str());
    }
    return 0;
}

int RunConfigSet(const std::string& path, const std::string& value_arg, bool from_stdin, Formatter& fmt, const CliContext& ctx) {
    std::string value = value_arg;
    if (from_stdin) {
        std::string line, all;
        while (std::getline(std::cin, line)) {
            if (!all.empty()) all += '\n';
            all += line;
        }
        value = all;
    }
    std::string error;
    if (!vinput::config::SetConfigValue(path, value, &error)) {
        fmt.PrintError(error);
        return 1;
    }
    fmt.PrintSuccess(_(msgs::kConfigValueSet, ctx.use_chinese));
    return 0;
}

int RunConfigEdit(const std::string& target, Formatter& fmt, const CliContext& ctx) {
    (void)fmt;
    (void)ctx;
    auto file_path = vinput::config::GetEditTarget(target);
    return OpenInEditor(file_path);
}
