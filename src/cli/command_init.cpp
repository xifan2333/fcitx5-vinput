#include "cli/command_init.h"

#include <filesystem>

#include "common/core_config.h"
#include "common/path_utils.h"

namespace fs = std::filesystem;

int RunInit(bool force, Formatter& fmt, const CliContext& ctx) {
    bool any_created = false;

    // 1. Create config.json with defaults
    auto config_path = vinput::path::CoreConfigPath();
    if (fs::exists(config_path) && !force) {
        fmt.PrintInfo(ctx.use_chinese ? "配置文件已存在: " + config_path.string()
                                      : "Config already exists: " + config_path.string());
    } else {
        CoreConfig config;
        config.scenes.activeScene = "default";
        config.scenes.definitions = {
            vinput::scene::Definition{
                .id = "default",
                .label = "默认",
                .llm = false,
                .prompt = "",
            },
        };
        if (SaveCoreConfig(config)) {
            fmt.PrintSuccess(ctx.use_chinese ? "已创建配置文件: " + config_path.string()
                                             : "Created config: " + config_path.string());
            any_created = true;
        } else {
            fmt.PrintError(ctx.use_chinese ? "创建配置文件失败" : "Failed to create config");
            return 1;
        }
    }

    // 2. Create model base directory
    auto model_dir = vinput::path::DefaultModelBaseDir();
    if (fs::exists(model_dir)) {
        fmt.PrintInfo(ctx.use_chinese ? "模型目录已存在: " + model_dir.string()
                                      : "Model dir already exists: " + model_dir.string());
    } else {
        std::error_code ec;
        fs::create_directories(model_dir, ec);
        if (ec) {
            fmt.PrintError(ctx.use_chinese ? "创建模型目录失败: " + ec.message()
                                           : "Failed to create model dir: " + ec.message());
            return 1;
        }
        fmt.PrintSuccess(ctx.use_chinese ? "已创建模型目录: " + model_dir.string()
                                         : "Created model dir: " + model_dir.string());
        any_created = true;
    }

    if (!any_created) {
        fmt.PrintInfo(ctx.use_chinese ? "所有文件已就绪，无需初始化"
                                      : "Everything already initialized");
    }

    return 0;
}
