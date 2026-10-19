#include "common/utils/sandbox.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <sys/stat.h>

#include "common/utils/path_utils.h"

namespace vinput::sandbox {

namespace {

constexpr const char* kSandboxInfoPath = "/.flatpak-info";
constexpr const char* kFlatpakAppId = "org.fcitx.Fcitx5";

std::string ReadFileContents(const std::filesystem::path& path) {
  std::ifstream f(path);
  if (!f)
    return {};
  return {std::istreambuf_iterator<char>(f), {}};
}

} // namespace

namespace detail {

bool DetectSandbox(const char* flatpak_id, const char* container, const char* info_path) {
  if (flatpak_id != nullptr && *flatpak_id != '\0') {
    return true;
  }
  if (container != nullptr && std::string_view(container) == "flatpak") {
    return true;
  }
  if (info_path != nullptr && *info_path != '\0') {
    struct stat st;
    return stat(info_path, &st) == 0;
  }
  return false;
}

} // namespace detail

bool IsInSandbox() {
  static const bool in_sandbox =
      detail::DetectSandbox(std::getenv("FLATPAK_ID"), std::getenv("container"), kSandboxInfoPath);
  return in_sandbox;
}

std::vector<std::string> WrapHostCommand(std::vector<std::string> args) {
  if (!IsInSandbox())
    return args;
  std::vector<std::string> wrapped = {"flatpak-spawn", "--host"};
  wrapped.insert(wrapped.end(), std::make_move_iterator(args.begin()),
                 std::make_move_iterator(args.end()));
  return wrapped;
}

std::vector<std::string> DaemonLogFilter() {
  if (IsInSandbox()) {
    return {"journalctl", "--user", "-t", "flatpak", "--grep", "vinput"};
  }
  return {"journalctl", "--user-unit", std::string(vinput::path::DaemonServiceUnitName())};
}

std::vector<std::string> MissingSandboxPermissions() {
  std::vector<std::string> missing;
  if (!IsInSandbox())
    return missing;

  std::string info = ReadFileContents(kSandboxInfoPath);
  if (info.find("pipewire") == std::string::npos)
    missing.emplace_back("pipewire");
  if (info.find("xdg-config/systemd") == std::string::npos)
    missing.emplace_back("xdg-config/systemd");
  if (info.find("xdg-cache") == std::string::npos)
    missing.emplace_back("xdg-cache");

  return missing;
}

std::string RewriteServiceUnit(const std::string& content) {
  if (!IsInSandbox())
    return content;

  std::string result = content;
  auto pos = result.find("ExecStart=");
  if (pos == std::string::npos)
    return result;

  auto end = result.find('\n', pos);
  const auto daemon_path = vinput::path::DaemonExecutablePath();
  result.replace(pos, end - pos,
                 "ExecStart=flatpak run --command=" + daemon_path.string() + " " + kFlatpakAppId +
                     "\n"
                     "ExecStop=pkill -INT vinput-daemon");
  return result;
}

} // namespace vinput::sandbox
