#include "common/utils/sandbox.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/string_utils.h"

namespace vinput::sandbox {

namespace {

constexpr const char* kSandboxInfoPath = "/.flatpak-info";
constexpr const char* kFlatpakAppId = "org.fcitx.Fcitx5";

std::filesystem::path DefaultPulseSocketPath(const char* xdg_runtime_dir, uint32_t uid) {
  if (xdg_runtime_dir != nullptr && *xdg_runtime_dir != '\0') {
    return std::filesystem::path(xdg_runtime_dir) / "pulse" / "native";
  }
  return std::filesystem::path("/run/user") / std::to_string(uid) / "pulse" / "native";
}

std::string_view ExtractFirstServerEntry(std::string_view raw) {
  std::string_view server = vinput::str::Trim(raw);
  const std::size_t space_pos = server.find_first_of(" \t");
  if (space_pos != std::string_view::npos) {
    server = server.substr(0, space_pos);
  }
  if (server.starts_with('{')) {
    const std::size_t close_brace = server.find('}');
    if (close_brace != std::string_view::npos) {
      server.remove_prefix(close_brace + 1);
    }
  }
  return server;
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
    std::error_code ec;
    return std::filesystem::exists(info_path, ec) && !ec;
  }
  return false;
}

PulseEndpoint ResolvePulseEndpoint(const char* pulse_server, const char* xdg_runtime_dir,
                                   uint32_t uid) {
  if (pulse_server == nullptr) {
    return PulseEndpoint{
        .socket_path = DefaultPulseSocketPath(xdg_runtime_dir, uid),
        .tcp_configured = false,
    };
  }

  std::string_view server = ExtractFirstServerEntry(pulse_server);
  if (server.starts_with("tcp:") || server.starts_with("tcp4:") || server.starts_with("tcp6:")) {
    return PulseEndpoint{.socket_path = {}, .tcp_configured = true};
  }
  if (server.starts_with("unix:")) {
    server.remove_prefix(5);
  }
  if (!server.empty() && server.front() == '/') {
    return PulseEndpoint{.socket_path = std::filesystem::path(server), .tcp_configured = false};
  }
  return PulseEndpoint{
      .socket_path = DefaultPulseSocketPath(xdg_runtime_dir, uid),
      .tcp_configured = false,
  };
}

std::vector<std::string> ProbeMissingPermissions(const ProbeOptions& options) {
  std::vector<std::string> missing;

  if (!options.pulse_tcp_configured &&
      (options.pulse_socket_path.empty() ||
       ::access(options.pulse_socket_path.c_str(), R_OK | W_OK) != 0)) {
    missing.emplace_back("pulseaudio");
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
  if (!IsInSandbox()) {
    return args;
  }
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
  if (!IsInSandbox()) {
    return {};
  }

  static const std::vector<std::string> missing = []() {
    const auto endpoint = detail::ResolvePulseEndpoint(std::getenv("PULSE_SERVER"),
                                                       std::getenv("XDG_RUNTIME_DIR"), ::getuid());
    detail::ProbeOptions options;
    options.pulse_socket_path = endpoint.socket_path;
    options.pulse_tcp_configured = endpoint.tcp_configured;
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
    if (perm == "pulseaudio") {
      result += "flatpak override --user --socket=pulseaudio ";
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
  if (!IsInSandbox()) {
    return content;
  }

  std::string result = content;
  auto pos = result.find("ExecStart=");
  if (pos == std::string::npos) {
    return result;
  }

  auto end = result.find('\n', pos);
  const auto daemon_path = vinput::path::DaemonExecutablePath();
  result.replace(pos, end - pos,
                 "ExecStart=flatpak run --command=" + daemon_path.string() + " " + kFlatpakAppId +
                     "\n"
                     "ExecStop=pkill -INT vinput-daemon");
  return result;
}

} // namespace vinput::sandbox
