#include "common/utils/sandbox.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace vinput::sandbox {

namespace {

constexpr const char* kSandboxInfoPath = "/.flatpak-info";
constexpr const char* kFlatpakAppId = "org.fcitx.Fcitx5";

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
    std::error_code ec;
    return std::filesystem::exists(info_path, ec) && !ec;
  }
  return false;
}

std::vector<std::string> ProbeMissingPermissions(const ProbeOptions& options) {
  std::vector<std::string> missing;

  if (options.pipewire_socket_path.empty() ||
      ::access(options.pipewire_socket_path.c_str(), R_OK | W_OK) != 0) {
    missing.emplace_back("pipewire");
  }

  if (!vinput::file::IsDirectoryWritable(options.systemd_dir)) {
    missing.emplace_back("xdg-config/systemd");
  }

  if (!vinput::file::IsDirectoryWritable(options.cache_dir)) {
    missing.emplace_back("xdg-cache");
  }

  return missing;
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
  if (!IsInSandbox())
    return {};

  static const std::vector<std::string> missing = []() {
    detail::ProbeOptions options;
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir != nullptr && *runtime_dir != '\0') {
      options.pipewire_socket_path = std::filesystem::path(runtime_dir) / "pipewire-0";
    } else {
      options.pipewire_socket_path =
          std::filesystem::path("/run/user") / std::to_string(::getuid()) / "pipewire-0";
    }
    options.systemd_dir = vinput::path::UserSystemdUnitDir();
    options.cache_dir = vinput::path::VinputCacheDir();
    return detail::ProbeMissingPermissions(options);
  }();

  return missing;
}

std::string FormatOverrideCommands(const std::vector<std::string>& missing_permissions) {
  if (missing_permissions.empty()) {
    return {};
  }
  std::string result;
  for (const auto& perm : missing_permissions) {
    if (perm == "pipewire") {
      result += "flatpak override --user --filesystem=xdg-run/pipewire-0 ";
      result += kFlatpakAppId;
      result += '\n';
    } else if (perm == "xdg-config/systemd") {
      result += "flatpak override --user --filesystem=xdg-config/systemd:create ";
      result += kFlatpakAppId;
      result += '\n';
    } else if (perm == "xdg-cache") {
      result += "flatpak override --user --filesystem=xdg-cache ";
      result += kFlatpakAppId;
      result += '\n';
    }
  }
  if (!result.empty()) {
    result += "flatpak kill ";
    result += kFlatpakAppId;
  }
  return result;
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
