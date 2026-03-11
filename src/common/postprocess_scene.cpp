#include "common/postprocess_scene.h"

#include <string>
#include <string_view>

namespace vinput::scene {

const Definition *Find(const Config &config, std::string_view scene_id) {
  for (const auto &scene : config.scenes) {
    if (scene.id == scene_id) {
      return &scene;
    }
  }
  return nullptr;
}

const Definition &Resolve(const Config &config, std::string_view scene_id) {
  if (const auto *scene = Find(config, scene_id)) {
    return *scene;
  }
  if (const auto *scene = Find(config, config.activeSceneId)) {
    return *scene;
  }
  static const Definition empty;
  return empty;
}

std::string DisplayLabel(const Definition &scene) {
  if (!scene.label.empty()) {
    return scene.label;
  }
  return scene.id;
}

bool AddScene(Config *config, const Definition &def, std::string *error) {
  if (def.id.empty()) {
    if (error)
      *error = "Scene id must not be empty.";
    return false;
  }
  if (Find(*config, def.id)) {
    if (error)
      *error = "Scene id '" + def.id + "' already exists.";
    return false;
  }
  config->scenes.push_back(def);
  return true;
}

bool UpdateScene(Config *config, const std::string &id, const Definition &def,
                 std::string *error) {
  for (auto &scene : config->scenes) {
    if (scene.id == id) {
      scene.label = def.label;
      scene.prompt = def.prompt;
      return true;
    }
  }
  if (error)
    *error = "Scene id '" + id + "' not found.";
  return false;
}

bool RemoveScene(Config *config, const std::string &id, bool force,
                 std::string *error) {
  if (!force) {
    if (config->activeSceneId == id) {
      if (error)
        *error = "Cannot remove active scene '" + id + "'.";
      return false;
    }
    // Assuming we still want to protect builtin scenes but `IsBuiltinScene` is
    // gone. The original code had IsBuiltinScene check, but the current code
    // does not have IsBuiltinScene.
    if (id == "default" || id == "formal" || id == "code" ||
        id == "translate") {
      if (error)
        *error = "Cannot remove built-in scene '" + id + "'.";
      return false;
    }
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
  if (error)
    *error = "Scene id '" + id + "' not found.";
  return false;
}

bool SetActiveScene(Config *config, const std::string &id, std::string *error) {
  if (!Find(*config, id)) {
    if (error)
      *error = "Scene id '" + id + "' not found.";
    return false;
  }
  config->activeSceneId = id;
  return true;
}

} // namespace vinput::scene
