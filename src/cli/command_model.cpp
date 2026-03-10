#include "cli/command_model.h"
#include "cli/editor_utils.h"
#include "cli/i18n.h"
#include "cli/progress.h"
#include "common/core_config.h"
#include "common/model_manager.h"
#include "common/model_repository.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sys/wait.h>
#include <unistd.h>

namespace {

static std::string FormatSize(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    }
    return buf;
}

static uint64_t DirSize(const std::filesystem::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (!ec && entry.is_regular_file(ec) && !ec) {
            total += entry.file_size(ec);
            ec.clear();
        }
    }
    return total;
}

} // namespace

int RunModelList(bool remote, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config.core);
    ModelManager mgr(base_dir.string());

    if (!remote) {
        auto models = mgr.ListDetailed(config.core.activeModel);

        if (ctx.json_output) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& m : models) {
                std::string state_str;
                if (m.state == ModelState::Active) state_str = "active";
                else if (m.state == ModelState::Broken) state_str = "broken";
                else state_str = "installed";
                arr.push_back({
                    {"name", m.name},
                    {"model_type", m.model_type},
                    {"language", m.language},
                    {"status", state_str}
                });
            }
            fmt.PrintJson(arr);
            return 0;
        }

        std::vector<std::string> headers = {"NAME", "TYPE", "LANGUAGE", "STATUS"};
        std::vector<std::vector<std::string>> rows;
        for (const auto& m : models) {
            std::string status_str;
            if (m.state == ModelState::Active) {
                status_str = std::string("[*] ") + _(msgs::kActive, ctx.use_chinese);
            } else if (m.state == ModelState::Broken) {
                status_str = std::string("[!] ") + _(msgs::kBroken, ctx.use_chinese);
            } else {
                status_str = std::string("[ ] ") + _(msgs::kInstalled, ctx.use_chinese);
            }
            rows.push_back({m.name, m.model_type, m.language, status_str});
        }
        fmt.PrintTable(headers, rows);
        return 0;
    }

    // Remote listing
    if (config.core.registryUrl.empty()) {
        fmt.PrintError("No registry URL configured. Set with: vinput config set extra.core.registry_url <url>");
        return 1;
    }

    ModelRepository repo(base_dir.string());
    std::string err;
    auto remote_models = repo.FetchRegistry(config.core.registryUrl, &err);
    if (!err.empty()) {
        fmt.PrintError(err);
        return 1;
    }

    // Get local model names for comparison
    auto local_models = mgr.ListDetailed(config.core.activeModel);
    auto is_installed = [&](const std::string& name) {
        for (const auto& lm : local_models) {
            if (lm.name == name) return true;
        }
        return false;
    };

    if (ctx.json_output) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : remote_models) {
            arr.push_back({
                {"name", m.name},
                {"display_name", m.display_name},
                {"model_type", m.model_type},
                {"language", m.language},
                {"size", FormatSize(m.size_bytes)},
                {"size_bytes", m.size_bytes},
                {"status", is_installed(m.name) ? "installed" : "available"},
                {"description", m.description}
            });
        }
        fmt.PrintJson(arr);
        return 0;
    }

    std::vector<std::string> headers = {"NAME", "TYPE", "LANGUAGE", "SIZE", "STATUS"};
    std::vector<std::vector<std::string>> rows;
    for (const auto& m : remote_models) {
        std::string status = is_installed(m.name) ? "installed" : "available";
        rows.push_back({m.name, m.model_type, m.language, FormatSize(m.size_bytes), status});
    }
    fmt.PrintTable(headers, rows);
    return 0;
}

int RunModelAdd(const std::string& name, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config.core);

    if (config.core.registryUrl.empty()) {
        fmt.PrintError("No registry URL configured. Set with: vinput config set extra.core.registry_url <url>");
        return 1;
    }

    ModelRepository repo(base_dir.string());

    // Fetch registry first to get total size for progress bar
    std::string err;
    auto remote_models = repo.FetchRegistry(config.core.registryUrl, &err);
    if (!err.empty()) {
        fmt.PrintError(err);
        return 1;
    }

    uint64_t total_size = 0;
    for (const auto& m : remote_models) {
        if (m.name == name) {
            total_size = m.size_bytes;
            break;
        }
    }

    char label_buf[256];
    snprintf(label_buf, sizeof(label_buf), _(msgs::kDownloading, ctx.use_chinese), name.c_str());
    ProgressBar bar(label_buf, total_size, ctx.is_tty);

    bool install_ok = repo.InstallModel(
        config.core.registryUrl, name,
        [&](const InstallProgress& p) {
            bar.Update(p.downloaded_bytes, p.speed_bps);
        },
        &err);

    bar.Finish();

    if (!install_ok) {
        fmt.PrintError(err);
        return 1;
    }

    char success_buf[256];
    snprintf(success_buf, sizeof(success_buf), _(msgs::kInstallSuccess, ctx.use_chinese), name.c_str());
    fmt.PrintSuccess(success_buf);
    fmt.PrintInfo("Run `vinput model use " + name + "` to activate");
    return 0;
}

int RunModelEdit(const std::string& name, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config.core);
    auto json_path = base_dir / name / "vinput-model.json";

    if (!std::filesystem::exists(json_path)) {
        fmt.PrintError("Model '" + name + "' not found at: " + json_path.string());
        return 1;
    }

    return OpenInEditor(json_path);
}

int RunModelUse(const std::string& name, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config.core);

    ModelManager mgr(base_dir.string());
    std::string err;
    if (!mgr.Validate(name, &err)) {
        fmt.PrintError("Model '" + name + "' is not valid: " + err);
        return 1;
    }

    config.core.activeModel = name;
    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save configuration.");
        return 1;
    }

    // Restart daemon via systemctl
    const char* argv[] = {"systemctl", "--user", "restart", "vinput-daemon.service", nullptr};
    pid_t pid = fork();
    if (pid == 0) {
        execvp("systemctl", const_cast<char* const*>(argv));
        _exit(127);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }

    fmt.PrintSuccess("Active model set to '" + name + "'. Daemon restarted.");
    return 0;
}

int RunModelRemove(const std::string& name, bool force, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config.core);

    if (name == config.core.activeModel && !force) {
        fmt.PrintError("Cannot remove active model '" + name + "'. Use --force to override.");
        return 1;
    }

    ModelManager mgr(base_dir.string());
    std::string err;
    if (!mgr.Remove(name, &err)) {
        fmt.PrintError(err);
        return 1;
    }

    fmt.PrintSuccess("Model '" + name + "' removed.");
    return 0;
}

int RunModelInfo(const std::string& name, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config.core);
    auto model_dir = base_dir / name;
    auto json_path = model_dir / "vinput-model.json";

    if (!std::filesystem::exists(json_path)) {
        fmt.PrintError("Model '" + name + "' not found at: " + json_path.string());
        return 1;
    }

    nlohmann::json j;
    {
        std::ifstream f(json_path);
        if (!f) {
            fmt.PrintError("Failed to read: " + json_path.string());
            return 1;
        }
        try {
            f >> j;
        } catch (const std::exception& e) {
            fmt.PrintError(std::string("Failed to parse vinput-model.json: ") + e.what());
            return 1;
        }
    }

    uint64_t dir_size = DirSize(model_dir);

    if (ctx.json_output) {
        nlohmann::json out = j;
        out["name"] = name;
        out["size_bytes"] = dir_size;
        out["size"] = FormatSize(dir_size);
        out["path"] = model_dir.string();
        fmt.PrintJson(out);
        return 0;
    }

    fmt.PrintKeyValue("Name", name);
    fmt.PrintKeyValue("Path", model_dir.string());
    fmt.PrintKeyValue("Size", FormatSize(dir_size));

    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string()) {
            fmt.PrintKeyValue(it.key(), it.value().get<std::string>());
        } else if (it.value().is_boolean()) {
            fmt.PrintKeyValue(it.key(), it.value().get<bool>() ? "true" : "false");
        } else if (it.value().is_number()) {
            fmt.PrintKeyValue(it.key(), it.value().dump());
        }
    }

    return 0;
}
