#include "common/postprocess_scene.h"

#include "common/i18n.h"

#include <fcitx-utils/standardpath.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {

using json = nlohmann::json;

vinput::scene::Config BuildDefaultConfig() {
    using vinput::scene::Config;
    using vinput::scene::Definition;

    Config config;
    config.activeSceneId = vinput::scene::kDefault;
    config.scenes = {
        Definition{
            .id = vinput::scene::kDefault,
            .label = _("Default"),
            .prompt = "",
        }
    };
    return config;
}

std::string JsonString(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

bool JsonBool(const json& object, const char* key, bool fallback) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

bool ParseSceneDefinition(const json& value, std::size_t index,
                          vinput::scene::Definition* out) {
    if (!value.is_object()) {
        fprintf(stderr,
                "vinput: skipping scene[%zu] in %s because it is not an object\n",
                index, vinput::scene::kConfigPath);
        return false;
    }

    vinput::scene::Definition scene;
    scene.id = JsonString(value, "id");
    if (scene.id.empty()) {
        fprintf(stderr,
                "vinput: skipping scene[%zu] in %s because id is missing\n",
                index, vinput::scene::kConfigPath);
        return false;
    }

    scene.label = JsonString(value, "label");
    scene.prompt = JsonString(value, "prompt");

    *out = std::move(scene);
    return true;
}

void NormalizeConfig(vinput::scene::Config* config) {
    if (config->scenes.empty()) {
        *config = vinput::scene::DefaultConfig();
        return;
    }

    if (config->activeSceneId.empty() ||
        !vinput::scene::Find(*config, config->activeSceneId)) {
        config->activeSceneId = config->scenes.front().id;
    }
}

}  // namespace

namespace vinput::scene {

const Config& DefaultConfig() {
    static const Config config = BuildDefaultConfig();
    return config;
}

Config LoadConfig() {
    auto path = fcitx::StandardPath::global().locate(
        fcitx::StandardPath::Type::PkgConfig, kConfigPath);
    if (path.empty()) {
        path = fcitx::StandardPath::global().locate(
            fcitx::StandardPath::Type::PkgData, kPkgDataPath);
    }
    if (path.empty()) {
        return DefaultConfig();
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        fprintf(stderr, "vinput: failed to open scene config: %s\n",
                path.c_str());
        return DefaultConfig();
    }

    json root;
    try {
        input >> root;
    } catch (const std::exception& e) {
        fprintf(stderr, "vinput: failed to parse scene config %s: %s\n",
                path.c_str(), e.what());
        return DefaultConfig();
    }

    if (!root.is_object()) {
        fprintf(stderr, "vinput: scene config %s is not a JSON object\n",
                path.c_str());
        return DefaultConfig();
    }

    const auto version_it = root.find("version");
    if (version_it != root.end() && version_it->is_number_integer() &&
        version_it->get<int>() != 1) {
        fprintf(stderr,
                "vinput: unsupported scene config version in %s, falling back "
                "to defaults\n",
                path.c_str());
        return DefaultConfig();
    }

    const auto scenes_it = root.find("scenes");
    if (scenes_it == root.end() || !scenes_it->is_array()) {
        fprintf(stderr,
                "vinput: scene config %s does not contain a valid scenes "
                "array\n",
                path.c_str());
        return DefaultConfig();
    }

    Config config;
    config.activeSceneId = JsonString(root, "active_scene");

    std::unordered_set<std::string> ids;
    for (std::size_t i = 0; i < scenes_it->size(); ++i) {
        Definition scene;
        if (!ParseSceneDefinition((*scenes_it)[i], i, &scene)) {
            continue;
        }

        if (!ids.insert(scene.id).second) {
            fprintf(stderr,
                    "vinput: skipping duplicate scene id '%s' in %s\n",
                    scene.id.c_str(), path.c_str());
            continue;
        }

        config.scenes.push_back(std::move(scene));
    }

    NormalizeConfig(&config);
    return config;
}

const Definition* Find(const Config& config, std::string_view scene_id) {
    for (const auto& scene : config.scenes) {
        if (scene.id == scene_id) {
            return &scene;
        }
    }
    return nullptr;
}

const Definition& Resolve(const Config& config, std::string_view scene_id) {
    if (const auto* scene = Find(config, scene_id)) {
        return *scene;
    }
    if (const auto* scene = Find(config, config.activeSceneId)) {
        return *scene;
    }
    return config.scenes.front();
}

std::string DisplayLabel(const Definition& scene) {
    if (!scene.label.empty()) {
        return scene.label;
    }
    if (scene.id == kDefault) return _("Default");
    if (scene.id == kFormal) return _("Formal");
    if (scene.id == kCode) return _("Code");
    if (scene.id == kTranslate) return _("Translate");
    return scene.id;
}

bool IsBuiltinScene(const std::string& id) {
    return id == kDefault || id == kFormal || id == kCode || id == kTranslate;
}

bool AddScene(Config* config, const Definition& def, std::string* error) {
    if (def.id.empty()) {
        if (error) *error = "Scene id must not be empty.";
        return false;
    }
    if (Find(*config, def.id)) {
        if (error) *error = "Scene id '" + def.id + "' already exists.";
        return false;
    }
    config->scenes.push_back(def);
    return true;
}

bool UpdateScene(Config* config, const std::string& id, const Definition& def,
                 std::string* error) {
    for (auto& scene : config->scenes) {
        if (scene.id == id) {
            scene.label = def.label;
            scene.prompt = def.prompt;
            return true;
        }
    }
    if (error) *error = "Scene id '" + id + "' not found.";
    return false;
}

bool RemoveScene(Config* config, const std::string& id, bool force,
                 std::string* error) {
    if (IsBuiltinScene(id) && !force) {
        if (error) {
            *error = "Cannot remove built-in scene '" + id +
                     "'. Use --force to override.";
        }
        return false;
    }
    for (auto it = config->scenes.begin(); it != config->scenes.end(); ++it) {
        if (it->id == id) {
            config->scenes.erase(it);
            if (config->activeSceneId == id) {
                config->activeSceneId =
                    config->scenes.empty() ? "" : config->scenes.front().id;
            }
            return true;
        }
    }
    if (error) *error = "Scene id '" + id + "' not found.";
    return false;
}

bool SetActiveScene(Config* config, const std::string& id, std::string* error) {
    if (!Find(*config, id)) {
        if (error) *error = "Scene id '" + id + "' not found.";
        return false;
    }
    config->activeSceneId = id;
    return true;
}

}  // namespace vinput::scene
