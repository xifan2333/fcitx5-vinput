#include "cli/command_init.h"

#include <filesystem>
#include <cstdio>

#include "common/core_config.h"
#include "common/i18n.h"

namespace {
static std::string FormatMsg1(const char* tmpl, const std::string& a) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), tmpl, a.c_str());
    return buf;
}
} // namespace
#include "common/path_utils.h"

namespace fs = std::filesystem;

int RunInit(bool force, Formatter& fmt, const CliContext& ctx) {
    (void)ctx;
    bool any_created = false;

    // 1. Create config.json with defaults
    auto config_path = vinput::path::CoreConfigPath();
    if (fs::exists(config_path) && !force) {
        fmt.PrintInfo(
            FormatMsg1(_("Config already exists: %s"), config_path.string()));
    } else {
        CoreConfig config;
        config.scenes.activeScene = "default";
        config.scenes.definitions = {
            vinput::scene::Definition{
                .id = "default",
                .label = "",
                .prompt = "",
            },
        };
        if (SaveCoreConfig(config)) {
            fmt.PrintSuccess(
                FormatMsg1(_("Created config: %s"), config_path.string()));
            any_created = true;
        } else {
            fmt.PrintError(_("Failed to create config"));
            return 1;
        }
    }

    // 2. Create model base directory
    auto model_dir = vinput::path::DefaultModelBaseDir();
    if (fs::exists(model_dir)) {
        fmt.PrintInfo(
            FormatMsg1(_("Model dir already exists: %s"), model_dir.string()));
    } else {
        std::error_code ec;
        fs::create_directories(model_dir, ec);
        if (ec) {
            fmt.PrintError(
                FormatMsg1(_("Failed to create model dir: %s"), ec.message()));
            return 1;
        }
        fmt.PrintSuccess(
            FormatMsg1(_("Created model dir: %s"), model_dir.string()));
        any_created = true;
    }

    if (!any_created) {
        fmt.PrintInfo(_("Everything already initialized"));
    }

    return 0;
}
