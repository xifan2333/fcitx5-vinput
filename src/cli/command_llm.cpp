#include "cli/command_llm.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "cli/editor_utils.h"
#include "cli/i18n.h"
#include "common/core_config.h"
#include "common/path_utils.h"

static std::string MaskApiKey(const std::string& key) {
    if (key.size() <= 8) return std::string(key.size(), '*');
    return key.substr(0, 4) + std::string(key.size() - 8, '*') + key.substr(key.size() - 4);
}

static std::string FormatMsg(const char* tmpl, const std::string& arg) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), tmpl, arg.c_str());
    return buf;
}

int RunLlmList(Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    const auto& providers = config.llm.providers;

    if (ctx.json_output) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : providers) {
            arr.push_back({
                {"name", p.name},
                {"base_url", p.base_url},
                {"model", p.model},
                {"api_key", ""},
                {"active", p.name == config.llm.activeProvider}
            });
        }
        fmt.PrintJson(arr);
        return 0;
    }

    std::vector<std::string> headers = {"NAME", "BASE_URL", "MODEL", "API_KEY"};
    std::vector<std::vector<std::string>> rows;
    for (const auto& p : providers) {
        std::string display_name = p.name;
        if (p.name == config.llm.activeProvider) display_name = "[*] " + p.name;
        rows.push_back({display_name, p.base_url, p.model, MaskApiKey(p.api_key)});
    }
    fmt.PrintTable(headers, rows);
    return 0;
}

int RunLlmAdd(const std::string& name, const std::string& base_url, const std::string& model, const std::string& api_key, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    for (const auto& p : config.llm.providers) {
        if (p.name == name) {
            fmt.PrintError(FormatMsg(_(msgs::kLlmProviderExists, ctx.use_chinese), name));
            return 1;
        }
    }

    LlmProvider provider;
    provider.name = name;
    provider.base_url = base_url;
    provider.model = model;
    provider.api_key = api_key;
    config.llm.providers.push_back(provider);

    // Auto-set active provider if this is the first
    if (config.llm.activeProvider.empty()) {
        config.llm.activeProvider = name;
    }

    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save config.");
        return 1;
    }
    fmt.PrintSuccess(FormatMsg(_(msgs::kLlmProviderAdded, ctx.use_chinese), name));
    return 0;
}

int RunLlmEdit(const std::string& name, Formatter& fmt, const CliContext& /*ctx*/) {
    auto config = LoadCoreConfig();

    // Find the provider
    LlmProvider* target = nullptr;
    for (auto& p : config.llm.providers) {
        if (p.name == name) {
            target = &p;
            break;
        }
    }
    if (!target) {
        fmt.PrintError(FormatMsg(_(msgs::kLlmProviderNotFound, false), name));
        return 1;
    }

    // Serialize to temp JSON file
    nlohmann::json j;
    j["name"] = target->name;
    j["base_url"] = target->base_url;
    j["model"] = target->model;
    j["api_key"] = target->api_key;
    j["candidate_count"] = target->candidate_count;
    j["timeout_ms"] = target->timeout_ms;

    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / ("vinput-llm-" + name + ".json");
    {
        std::ofstream out(tmp);
        if (!out.is_open()) {
            fmt.PrintError("Failed to create temp file: " + tmp.string());
            return 1;
        }
        out << j.dump(2) << "\n";
    }

    int ret = OpenInEditor(tmp);
    if (ret != 0) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return ret;
    }

    // Read back
    std::ifstream in(tmp);
    if (!in.is_open()) {
        fmt.PrintError("Failed to read back edited file.");
        return 1;
    }

    nlohmann::json edited;
    try {
        in >> edited;
    } catch (const std::exception& e) {
        fmt.PrintError(std::string("Invalid JSON: ") + e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
        return 1;
    }
    std::error_code ec;
    fs::remove(tmp, ec);

    // Apply changes (name is immutable)
    target->base_url = edited.value("base_url", target->base_url);
    target->model = edited.value("model", target->model);
    target->api_key = edited.value("api_key", target->api_key);
    target->candidate_count = edited.value("candidate_count", target->candidate_count);
    target->timeout_ms = edited.value("timeout_ms", target->timeout_ms);

    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save config.");
        return 1;
    }

    fmt.PrintSuccess(FormatMsg("Provider '%s' updated.", name));
    return 0;
}

int RunLlmUse(const std::string& name, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    bool found = false;
    for (const auto& p : config.llm.providers) {
        if (p.name == name) { found = true; break; }
    }
    if (!found) {
        fmt.PrintError(FormatMsg(_(msgs::kLlmProviderNotFound, ctx.use_chinese), name));
        return 1;
    }
    config.llm.activeProvider = name;
    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save config.");
        return 1;
    }
    fmt.PrintSuccess(FormatMsg(_(msgs::kLlmProviderSwitched, ctx.use_chinese), name));
    return 0;
}

int RunLlmRemove(const std::string& name, bool force, Formatter& fmt, const CliContext& ctx) {
    auto config = LoadCoreConfig();
    if (name == config.llm.activeProvider && !force) {
        fmt.PrintError(FormatMsg(_(msgs::kLlmCannotRemoveActive, ctx.use_chinese), name));
        return 1;
    }
    auto& providers = config.llm.providers;
    auto it = std::find_if(providers.begin(), providers.end(),
        [&name](const LlmProvider& p) { return p.name == name; });
    if (it == providers.end()) {
        fmt.PrintError(FormatMsg(_(msgs::kLlmProviderNotFound, ctx.use_chinese), name));
        return 1;
    }
    providers.erase(it);
    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save config.");
        return 1;
    }
    fmt.PrintSuccess(FormatMsg(_(msgs::kLlmProviderRemoved, ctx.use_chinese), name));
    return 0;
}
