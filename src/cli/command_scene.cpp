#include "cli/command_scene.h"
#include "cli/editor_utils.h"
#include "cli/i18n.h"
#include "common/core_config.h"
#include "common/file_utils.h"
#include "common/path_utils.h"
#include "common/postprocess_scene.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

vinput::scene::Config ToSceneConfig(const CoreConfig::Scenes& s) {
    vinput::scene::Config c;
    c.activeSceneId = s.activeScene;
    c.scenes = s.definitions;
    return c;
}

void FromSceneConfig(CoreConfig::Scenes& s, const vinput::scene::Config& c) {
    s.activeScene = c.activeSceneId;
    s.definitions = c.scenes;
}

} // namespace

int RunSceneList(Formatter& fmt, const CliContext& ctx) {
    CoreConfig config = LoadCoreConfig();
    const auto& scenes = config.scenes.definitions;

    if (ctx.json_output) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& scene : scenes) {
            bool active = (scene.id == config.scenes.activeScene);
            arr.push_back({
                {"id", scene.id},
                {"label", vinput::scene::DisplayLabel(scene, ctx.use_chinese)},
                {"llm", scene.llm},
                {"active", active}
            });
        }
        fmt.PrintJson(arr);
        return 0;
    }

    std::vector<std::string> headers = {"ID", "LABEL", "LLM", "STATUS"};
    std::vector<std::vector<std::string>> rows;
    for (const auto& scene : scenes) {
        std::string label = vinput::scene::DisplayLabel(scene, ctx.use_chinese);
        std::string llm_str = scene.llm ? "yes" : "no";
        std::string status = (scene.id == config.scenes.activeScene) ? "[*]" : "[ ]";
        rows.push_back({scene.id, label, llm_str, status});
    }
    fmt.PrintTable(headers, rows);
    return 0;
}

int RunSceneAdd(const std::string& id, const std::string& label,
                bool llm, const std::string& prompt,
                Formatter& fmt, const CliContext& ctx) {
    CoreConfig config = LoadCoreConfig();

    vinput::scene::Definition def;
    def.id = id;
    def.label = label;
    def.llm = llm;
    def.prompt = prompt;

    vinput::scene::Config scene_config = ToSceneConfig(config.scenes);
    std::string error;
    if (!vinput::scene::AddScene(&scene_config, def, &error)) {
        fmt.PrintError(error);
        return 1;
    }
    FromSceneConfig(config.scenes, scene_config);

    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save config.");
        return 1;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Scene '%s' added.", id.c_str());
    fmt.PrintSuccess(buf);
    return 0;
}

int RunSceneEdit(const std::string& id, Formatter& fmt, const CliContext& /*ctx*/) {
    // No ID given: open entire config in editor
    if (id.empty()) {
        return OpenInEditor(vinput::path::CoreConfigPath());
    }

    // ID given: extract that scene to a temp file, edit, merge back
    CoreConfig config = LoadCoreConfig();
    auto scene_config = ToSceneConfig(config.scenes);
    const auto* scene = vinput::scene::Find(scene_config, id);
    if (!scene) {
        fmt.PrintError("Scene '" + id + "' not found.");
        return 1;
    }

    nlohmann::json j;
    j["id"] = scene->id;
    j["label"] = scene->label;
    j["llm"] = scene->llm;
    j["prompt"] = scene->prompt;

    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / ("vinput-scene-" + id + ".json");
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

    // Read back edited content
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

    // Apply changes
    vinput::scene::Definition updated;
    updated.id = id; // ID is immutable
    updated.label = edited.value("label", scene->label);
    updated.llm = edited.value("llm", scene->llm);
    updated.prompt = edited.value("prompt", scene->prompt);

    std::string error;
    if (!vinput::scene::UpdateScene(&scene_config, id, updated, &error)) {
        fmt.PrintError(error);
        return 1;
    }
    FromSceneConfig(config.scenes, scene_config);

    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save config.");
        return 1;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Scene '%s' updated.", id.c_str());
    fmt.PrintSuccess(buf);
    return 0;
}

int RunSceneUse(const std::string& id, Formatter& fmt, const CliContext& ctx) {
    (void)ctx;
    CoreConfig config = LoadCoreConfig();

    vinput::scene::Config scene_config = ToSceneConfig(config.scenes);
    std::string error;
    if (!vinput::scene::SetActiveScene(&scene_config, id, &error)) {
        fmt.PrintError(error);
        return 1;
    }
    FromSceneConfig(config.scenes, scene_config);

    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save config.");
        return 1;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Default scene set to '%s'.", id.c_str());
    fmt.PrintSuccess(buf);
    return 0;
}

int RunSceneRemove(const std::string& id, bool force, Formatter& fmt, const CliContext& ctx) {
    (void)ctx;
    CoreConfig config = LoadCoreConfig();

    vinput::scene::Config scene_config = ToSceneConfig(config.scenes);
    std::string error;
    if (!vinput::scene::RemoveScene(&scene_config, id, force, &error)) {
        fmt.PrintError(error);
        return 1;
    }
    FromSceneConfig(config.scenes, scene_config);

    if (!SaveCoreConfig(config)) {
        fmt.PrintError("Failed to save config.");
        return 1;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Scene '%s' removed.", id.c_str());
    fmt.PrintSuccess(buf);
    return 0;
}
