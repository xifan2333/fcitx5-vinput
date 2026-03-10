#include "common/path_utils.h"
#include <cstdlib>

namespace vinput::path {

std::filesystem::path ExpandUserPath(std::string_view path) {
  if (path.empty() || path[0] != '~') {
    return std::filesystem::path(path);
  }
  const char* home = std::getenv("HOME");
  if (!home) home = "/tmp";
  return std::filesystem::path(home) / std::filesystem::path(path.substr(path.size() > 1 && path[1] == '/' ? 2 : 1));
}

std::filesystem::path DefaultModelBaseDir() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "vinput" / "models";
  }
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : "/tmp") / ".local" / "share" / "vinput" / "models";
}

std::filesystem::path CoreConfigPath() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "vinput" / "config.json";
  }
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : "/tmp") / ".config" / "vinput" / "config.json";
}

std::filesystem::path UserSceneConfigPath() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "fcitx5" / "conf" / "vinput-scenes.json";
  }
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : "/tmp") / ".config" / "fcitx5" / "conf" / "vinput-scenes.json";
}

}  // namespace vinput::path
