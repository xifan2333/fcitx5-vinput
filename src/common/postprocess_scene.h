#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vinput::scene {

inline constexpr const char* kDefault = "default";
inline constexpr const char* kFormal = "formal";
inline constexpr const char* kCode = "code";
inline constexpr const char* kTranslate = "translate";
inline constexpr const char* kConfigPath = "conf/vinput-scenes.json";
inline constexpr const char* kPkgDataPath = "vinput/scenes.json";

struct Definition {
    std::string id;
    std::string label;
    std::string prompt;
};

struct Config {
    std::string activeSceneId;
    std::vector<Definition> scenes;
};

const Config& DefaultConfig();
Config LoadConfig();
const Definition* Find(const Config& config, std::string_view scene_id);
const Definition& Resolve(const Config& config, std::string_view scene_id);
std::string DisplayLabel(const Definition& scene);

bool IsBuiltinScene(const std::string& id);
bool AddScene(Config* config, const Definition& def, std::string* error);
bool UpdateScene(Config* config, const std::string& id, const Definition& def, std::string* error);
bool RemoveScene(Config* config, const std::string& id, bool force, std::string* error);
bool SetActiveScene(Config* config, const std::string& id, std::string* error);

}  // namespace vinput::scene
