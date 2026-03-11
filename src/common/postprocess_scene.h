#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vinput::scene {

struct Definition {
  std::string id;
  std::string label;
  std::string prompt;
};

struct Config {
  std::string activeSceneId;
  std::vector<Definition> scenes;
};

const Definition *Find(const Config &config, std::string_view scene_id);
const Definition &Resolve(const Config &config, std::string_view scene_id);
std::string DisplayLabel(const Definition &scene);

bool AddScene(Config *config, const Definition &def, std::string *error);
bool UpdateScene(Config *config, const std::string &id, const Definition &def,
                 std::string *error);
bool RemoveScene(Config *config, const std::string &id, bool force,
                 std::string *error);
bool SetActiveScene(Config *config, const std::string &id, std::string *error);

} // namespace vinput::scene
