#include "common/config_path.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "common/file_utils.h"
#include "common/path_utils.h"

using json = nlohmann::json;

namespace vinput::config {

static const std::string kExtraPrefix = "extra.";

// Parse a dotpath like "extra.active_model" → vector ["active_model"]
// Returns false if prefix is not "extra."
static bool ParseExtraDotpath(const std::string &dotpath,
                              std::vector<std::string> *keys,
                              std::string *error) {
  if (dotpath.substr(0, kExtraPrefix.size()) != kExtraPrefix) {
    if (error)
      *error = "Only 'extra.*' dotpaths are supported. Got: " + dotpath;
    return false;
  }
  std::string rest = dotpath.substr(kExtraPrefix.size());
  if (rest.empty()) {
    if (error)
      *error = "Empty path after 'extra.'";
    return false;
  }
  keys->clear();
  std::istringstream ss(rest);
  std::string token;
  while (std::getline(ss, token, '.')) {
    if (token.empty()) {
      if (error)
        *error = "Empty segment in dotpath: " + dotpath;
      return false;
    }
    keys->push_back(token);
  }
  return true;
}

static json LoadRawJson(const std::filesystem::path &path) {
  std::ifstream f(path);
  if (!f.is_open())
    return json::object();
  try {
    json j;
    f >> j;
    return j;
  } catch (...) {
    return json::object();
  }
}

bool GetConfigValue(const std::string &dotpath, std::string *value,
                    std::string *error) {
  std::vector<std::string> keys;
  if (!ParseExtraDotpath(dotpath, &keys, error))
    return false;

  auto path = vinput::path::CoreConfigPath();
  json j = LoadRawJson(path);

  const json *cur = &j;
  for (const auto &key : keys) {
    if (!cur->is_object() || !cur->contains(key)) {
      if (error)
        *error = "Key not found: " + key;
      return false;
    }
    cur = &(*cur)[key];
  }

  if (cur->is_string()) {
    *value = cur->get<std::string>();
  } else if (cur->is_boolean()) {
    *value = cur->get<bool>() ? "true" : "false";
  } else if (cur->is_number_integer()) {
    *value = std::to_string(cur->get<int64_t>());
  } else if (cur->is_number_float()) {
    *value = std::to_string(cur->get<double>());
  } else {
    *value = cur->dump();
  }
  return true;
}

bool SetConfigValue(const std::string &dotpath, const std::string &value,
                    std::string *error) {
  std::vector<std::string> keys;
  if (!ParseExtraDotpath(dotpath, &keys, error))
    return false;

  auto path = vinput::path::CoreConfigPath();
  json j = LoadRawJson(path);

  // Navigate to parent and set the leaf
  json *cur = &j;
  for (size_t i = 0; i + 1 < keys.size(); ++i) {
    const auto &key = keys[i];
    if (!cur->is_object())
      *cur = json::object();
    if (!cur->contains(key) || !(*cur)[key].is_object()) {
      (*cur)[key] = json::object();
    }
    cur = &(*cur)[key];
  }

  const std::string &leaf = keys.back();
  // Type inference: bool, int, or string
  if (value == "true") {
    (*cur)[leaf] = true;
  } else if (value == "false") {
    (*cur)[leaf] = false;
  } else {
    try {
      size_t pos = 0;
      long long ival = std::stoll(value, &pos);
      if (pos == value.size()) {
        (*cur)[leaf] = static_cast<int64_t>(ival);
      } else {
        (*cur)[leaf] = value;
      }
    } catch (...) {
      (*cur)[leaf] = value;
    }
  }

  std::string err;
  if (!vinput::file::EnsureParentDirectory(path, &err)) {
    if (error)
      *error = "Failed to create config directory: " + err;
    return false;
  }

  std::string content = j.dump(4) + "\n";
  if (!vinput::file::AtomicWriteTextFile(path, content, &err)) {
    if (error)
      *error = "Failed to write config: " + err;
    return false;
  }
  return true;
}

std::filesystem::path GetEditTarget(const std::string &target) {
  if (target == "extra") {
    return vinput::path::CoreConfigPath();
  }
  // "fcitx" → $XDG_CONFIG_HOME/fcitx5/conf/vinput.conf
  //           or ~/.config/fcitx5/conf/vinput.conf
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "fcitx5" / "conf" / "vinput.conf";
  }
  const char *home = std::getenv("HOME");
  if (!home || home[0] == '\0')
    return {};
  return std::filesystem::path(home) / ".config" / "fcitx5" / "conf" / "vinput.conf";
}

} // namespace vinput::config
