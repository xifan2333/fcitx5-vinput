#include "cli/command_model.h"
#include "common/i18n.h"
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

static std::string FormatMsg1(const char* tmpl, const std::string& a) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), tmpl, a.c_str());
    return buf;
}

static std::string FormatMsg2(const char* tmpl, const std::string& a,
                              const std::string& b) {
    char buf[768];
    std::snprintf(buf, sizeof(buf), tmpl, a.c_str(), b.c_str());
    return buf;
}

} // namespace

int RunModelList(bool remote, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config);
    ModelManager mgr(base_dir.string());

    if (!remote) {
        auto models = mgr.ListDetailed(config.activeModel);

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
                    {"status", state_str},
                    {"supports_hotwords", m.supports_hotwords}
                });
            }
            fmt.PrintJson(arr);
            return 0;
        }

        std::vector<std::string> headers = {_("NAME"), _("TYPE"), _("LANGUAGE"), _("HOTWORDS"), _("STATUS")};
        std::vector<std::vector<std::string>> rows;
        for (const auto& m : models) {
            std::string status_str;
            if (m.state == ModelState::Active) {
                status_str = std::string("[*] ") + _("Active");
            } else if (m.state == ModelState::Broken) {
                status_str = std::string("[!] ") + _("Broken");
            } else {
                status_str = std::string("[ ] ") + _("Installed");
            }
            std::string hotwords = m.supports_hotwords ? _("yes") : _("no");
            rows.push_back({m.name, m.model_type, m.language, hotwords, status_str});
        }
        fmt.PrintTable(headers, rows);
        return 0;
    }

    // Remote listing
    if (config.registryUrl.empty()) {
        fmt.PrintError(_("No registry URL configured. Set with: vinput config set extra.registry_url <url>"));
        return 1;
    }

    ModelRepository repo(base_dir.string());
    std::string err;
    auto remote_models = repo.FetchRegistry(config.registryUrl, &err);
    if (!err.empty()) {
        fmt.PrintError(err);
        return 1;
    }

    // Get local model names for comparison
    auto local_models = mgr.ListDetailed(config.activeModel);
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
                {"supports_hotwords", m.supports_hotwords},
                {"description", m.description}
            });
        }
        fmt.PrintJson(arr);
        return 0;
    }

    std::vector<std::string> headers = {_("NAME"), _("TYPE"), _("LANGUAGE"), _("SIZE"), _("HOTWORDS"), _("STATUS")};
    std::vector<std::vector<std::string>> rows;
    for (const auto& m : remote_models) {
        std::string status = is_installed(m.name) ? _("installed") : _("available");
        std::string hotwords = m.supports_hotwords ? _("yes") : _("no");
        rows.push_back({m.name, m.model_type, m.language, FormatSize(m.size_bytes), hotwords, status});
    }
    fmt.PrintTable(headers, rows);
    return 0;
}

int RunModelAdd(const std::string& name, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config);

    if (config.registryUrl.empty()) {
        fmt.PrintError(_("No registry URL configured. Set with: vinput config set extra.registry_url <url>"));
        return 1;
    }

    ModelRepository repo(base_dir.string());

    // Fetch registry first to get total size for progress bar
    std::string err;
    auto remote_models = repo.FetchRegistry(config.registryUrl, &err);
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
    snprintf(label_buf, sizeof(label_buf), _("Downloading %s..."), name.c_str());
    ProgressBar bar(label_buf, total_size, ctx.is_tty);

    bool install_ok = repo.InstallModel(
        config.registryUrl, name,
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
    snprintf(success_buf, sizeof(success_buf), _("Model '%s' installed successfully."), name.c_str());
    fmt.PrintSuccess(success_buf);
    fmt.PrintInfo(FormatMsg1(_("Run `vinput model use %s` to activate"), name));
    return 0;
}

int RunModelUse(const std::string& name, Formatter& fmt, const CliContext& /*ctx*/) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config);

    ModelManager mgr(base_dir.string());
    std::string err;
    if (!mgr.Validate(name, &err)) {
        fmt.PrintError(FormatMsg2(_("Model '%s' is not valid: %s"), name, err));
        return 1;
    }

    config.activeModel = name;
    if (!SaveCoreConfig(config)) {
        fmt.PrintError(_("Failed to save configuration."));
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

    fmt.PrintSuccess(FormatMsg1(_("Active model set to '%s'. Daemon restarted."), name));
    return 0;
}

int RunModelRemove(const std::string& name, bool force, Formatter& fmt, const CliContext& /*ctx*/) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config);

    if (name == config.activeModel && !force) {
        fmt.PrintError(FormatMsg1(_("Cannot remove active model '%s'. Use --force to override."), name));
        return 1;
    }

    ModelManager mgr(base_dir.string());
    std::string err;
    if (!mgr.Remove(name, &err)) {
        fmt.PrintError(err);
        return 1;
    }

    fmt.PrintSuccess(FormatMsg1(_("Model '%s' removed."), name));
    return 0;
}

int RunModelInfo(const std::string& name, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    NormalizeCoreConfig(&config);
    auto base_dir = ResolveModelBaseDir(config);
    auto model_dir = base_dir / name;
    auto json_path = model_dir / "vinput-model.json";

    if (!std::filesystem::exists(json_path)) {
        fmt.PrintError(FormatMsg2(_("Model '%s' not found at: %s"), name, json_path.string()));
        return 1;
    }

    nlohmann::json j;
    {
        std::ifstream f(json_path);
        if (!f) {
            fmt.PrintError(FormatMsg1(_("Failed to read: %s"), json_path.string()));
            return 1;
        }
        try {
            f >> j;
        } catch (const std::exception& e) {
            fmt.PrintError(FormatMsg1(_("Failed to parse vinput-model.json: %s"), e.what()));
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

    fmt.PrintKeyValue(_("Name"), name);
    fmt.PrintKeyValue(_("Path"), model_dir.string());
    fmt.PrintKeyValue(_("Size"), FormatSize(dir_size));

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
