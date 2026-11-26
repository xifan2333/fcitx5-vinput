#include "common/utils/sandbox.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
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

bool StripMachineQualifier(std::string_view& entry, std::string_view local_machine_id) {
  if (!entry.starts_with('{')) {
    return true;
  }
  const std::size_t close_brace = entry.find('}');
  if (close_brace == std::string_view::npos) {
    return false;
  }
  const std::string_view qualifier = entry.substr(1, close_brace - 1);
  if (local_machine_id.empty() || qualifier != local_machine_id) {
    return false;
  }
  entry.remove_prefix(close_brace + 1);
  return !entry.empty();
}

std::optional<detail::PulseEndpoint> ParseSinglePulseServer(std::string_view entry,
                                                            std::string_view local_machine_id) {
  if (!StripMachineQualifier(entry, local_machine_id) || entry.empty()) {
    return std::nullopt;
  }
  if (vinput::str::ConsumePrefix(&entry, "unix:")) {
    if (entry.empty()) {
      return std::nullopt;
    }
    return detail::PulseEndpoint{.socket_path = std::filesystem::path(entry),
                                 .tcp_configured = false};
  }
  if (entry.front() == '/') {
    return detail::PulseEndpoint{.socket_path = std::filesystem::path(entry),
                                 .tcp_configured = false};
  }

  static constexpr std::array<std::string_view, 3> kTcpPrefixes = {"tcp:", "tcp4:", "tcp6:"};
  for (const std::string_view prefix : kTcpPrefixes) {
    if (vinput::str::ConsumePrefix(&entry, prefix)) {
      break;
    }
  }
  if (entry.empty()) {
    return std::nullopt;
  }
  return detail::PulseEndpoint{.socket_path = {}, .tcp_configured = true};
}

bool IsPulseEndpointAccessible(const detail::PulseEndpoint& endpoint) noexcept {
  if (endpoint.tcp_configured) {
    return true;
  }
  return !endpoint.socket_path.empty() && ::access(endpoint.socket_path.c_str(), R_OK | W_OK) == 0;
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

std::string LocalPulseMachineId() {
  static constexpr std::array<const char*, 2> kMachineIdPaths = {
      "/etc/machine-id",
      "/var/lib/dbus/machine-id",
  };
  for (const char* path : kMachineIdPaths) {
    const auto content = vinput::file::ReadTextFile(path, 128);
    if (!content) {
      continue;
    }
    const std::string_view trimmed = vinput::str::Trim(*content);
    if (!trimmed.empty()) {
      return std::string(trimmed);
    }
  }

  std::array<char, 256> host_buf{};
  if (::gethostname(host_buf.data(), host_buf.size()) == 0) {
    host_buf.back() = '\0';
    if (host_buf[0] != '\0') {
      return {host_buf.data()};
    }
  }
  return {};
}

std::vector<PulseEndpoint> ResolvePulseEndpoints(const char* pulse_server,
                                                 const char* xdg_runtime_dir, uint32_t uid,
                                                 std::string_view local_machine_id) {
  if (pulse_server == nullptr) {
    return {PulseEndpoint{
        .socket_path = DefaultPulseSocketPath(xdg_runtime_dir, uid),
        .tcp_configured = false,
    }};
  }

  std::string_view remaining = vinput::str::Trim(pulse_server);
  if (remaining.empty()) {
    return {PulseEndpoint{
        .socket_path = DefaultPulseSocketPath(xdg_runtime_dir, uid),
        .tcp_configured = false,
    }};
  }

  std::vector<PulseEndpoint> endpoints;
  while (!remaining.empty()) {
    const std::size_t sep = remaining.find_first_of(" \t");
    const std::string_view token =
        (sep == std::string_view::npos) ? remaining : remaining.substr(0, sep);
    if (const auto parsed = ParseSinglePulseServer(token, local_machine_id)) {
      endpoints.push_back(*parsed);
    }
    if (sep == std::string_view::npos) {
      break;
    }
    remaining = vinput::str::Trim(remaining.substr(sep + 1));
  }
  return endpoints;
}

std::vector<std::string> ProbeMissingPermissions(const ProbeOptions& options) {
  std::vector<std::string> missing;

  if (!std::ranges::any_of(options.pulse_endpoints, IsPulseEndpointAccessible)) {
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
    detail::ProbeOptions options;
    options.pulse_endpoints =
        detail::ResolvePulseEndpoints(std::getenv("PULSE_SERVER"), std::getenv("XDG_RUNTIME_DIR"),
                                      ::getuid(), detail::LocalPulseMachineId());
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
