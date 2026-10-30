#include "common/utils/path_utils.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "common/utils/sandbox.h"

#include "config.h"

namespace vinput::path {

namespace {

constexpr std::string_view kDaemonServiceUnitName = "vinput-daemon.service";
constexpr std::string_view kDaemonExecutableName = "vinput-daemon";

std::optional<std::string_view> GetEnvNonEmpty(const char* name) {
  if (const char* val = std::getenv(name); val != nullptr && val[0] != '\0') {
    return std::string_view(val);
  }
  return std::nullopt;
}

std::filesystem::path FlatpakAddonRootDir() {
  return std::filesystem::path("/app") / "addons" / "Vinput";
}

std::filesystem::path FlatpakBundledSystemdUnitPath(std::string_view unit_name) {
  return FlatpakAddonRootDir() / "share" / "systemd" / "user" / std::filesystem::path(unit_name);
}

std::filesystem::path FlatpakBundledExecutablePath(std::string_view name) {
  return FlatpakAddonRootDir() / "bin" / std::filesystem::path(name);
}

std::filesystem::path UserSystemdUnitPath(std::string_view unit_name) {
  return vinput::path::UserSystemdUnitDir() / std::filesystem::path(unit_name);
}

std::filesystem::path XdgConfigHome() {
  if (const auto xdg = GetEnvNonEmpty("XDG_CONFIG_HOME")) {
    return {*xdg};
  }
  if (const auto home = GetEnvNonEmpty("HOME")) {
    return std::filesystem::path(*home) / ".config";
  }
  return {};
}

std::filesystem::path XdgDataHome() {
  if (const auto xdg = GetEnvNonEmpty("XDG_DATA_HOME")) {
    return {*xdg};
  }
  if (const auto home = GetEnvNonEmpty("HOME")) {
    return std::filesystem::path(*home) / ".local" / "share";
  }
  return {};
}

std::filesystem::path XdgCacheHome() {
  if (const auto xdg = GetEnvNonEmpty("XDG_CACHE_HOME")) {
    return {*xdg};
  }
  if (const auto home = GetEnvNonEmpty("HOME")) {
    return std::filesystem::path(*home) / ".cache";
  }
  return {};
}

} // namespace

bool IsPathWithinRoot(const std::filesystem::path& path, const std::filesystem::path& root) {
  const auto norm_path = path.lexically_normal();
  auto norm_root = root.lexically_normal();
  while (norm_root.filename().empty() && norm_root.has_parent_path() &&
         norm_root != norm_root.parent_path()) {
    norm_root = norm_root.parent_path();
  }
  auto path_it = norm_path.begin();
  auto root_it = norm_root.begin();
  for (; root_it != norm_root.end(); ++root_it, ++path_it) {
    if (path_it == norm_path.end() || *path_it != *root_it) {
      return false;
    }
  }
  return true;
}

std::filesystem::path VinputConfigDir() {
  return XdgConfigHome() / "vinput";
}
std::filesystem::path VinputDataDir() {
  return XdgDataHome() / "vinput";
}
std::filesystem::path VinputCacheDir() {
  return XdgCacheHome() / "vinput";
}

std::string_view DaemonServiceUnitName() {
  return kDaemonServiceUnitName;
}

std::filesystem::path DaemonExecutablePath() {
  if (vinput::sandbox::IsInSandbox()) {
    return FlatpakBundledExecutablePath(kDaemonExecutableName);
  }
  return {kDaemonExecutableName};
}

std::filesystem::path DaemonServiceUnitInstallPath() {
  return UserSystemdUnitPath(DaemonServiceUnitName());
}

std::filesystem::path DaemonServiceUnitTemplatePath() {
  if (vinput::sandbox::IsInSandbox()) {
    return FlatpakBundledSystemdUnitPath(DaemonServiceUnitName());
  }
  return {};
}

std::filesystem::path ExpandUserPath(std::string_view path) {
  if (path.empty() || path.front() != '~') {
    return {path};
  }
  const auto home = GetEnvNonEmpty("HOME");
  if (!home) {
    return {};
  }
  return std::filesystem::path(*home) /
         std::filesystem::path(path.substr(path.size() > 1 && path[1] == '/' ? 2 : 1));
}

std::filesystem::path DefaultModelBaseDir() {
  return VinputDataDir() / "models";
}

std::filesystem::path CoreConfigPath() {
  return VinputConfigDir() / "config.json";
}

std::filesystem::path FcitxAddonConfigPath() {
  const auto config_home = XdgConfigHome();
  if (config_home.empty()) {
    return {};
  }
  return config_home / "fcitx5" / "conf" / "vinput.conf";
}

std::filesystem::path RegistryCacheDir() {
  return VinputCacheDir() / "registry";
}

std::filesystem::path UserSystemdUnitDir() {
  return XdgConfigHome() / "systemd" / "user";
}

std::filesystem::path ManagedResourceDir(std::string_view category) {
  return VinputDataDir() / std::filesystem::path(category);
}

std::filesystem::path ManagedAsrProviderDir() {
  return ManagedResourceDir("providers");
}

std::filesystem::path ManagedLlmAdapterDir() {
  return ManagedResourceDir("adapters");
}

std::filesystem::path AdapterRuntimeDir() {
  if (const auto xdg_runtime = GetEnvNonEmpty("XDG_RUNTIME_DIR")) {
    return std::filesystem::path(*xdg_runtime) / "vinput" / "adapters";
  }

  const auto tmpdir = GetEnvNonEmpty("TMPDIR");
  const std::filesystem::path base =
      tmpdir ? std::filesystem::path(*tmpdir) : std::filesystem::path("/tmp");
  return base / "vinput" / "adapters";
}

std::filesystem::path ContextCachePath() {
  return VinputCacheDir() / "context.jsonl";
}

std::filesystem::path ReadNotificationsPath() {
  return VinputCacheDir() / "read_notifications";
}

std::filesystem::path ProviderModelCachePath() {
  return VinputCacheDir() / "provider_models.json";
}

std::filesystem::path InstalledManifestCachePath() {
  return VinputCacheDir() / "installed.json";
}

std::filesystem::path I18nCachePath(std::string_view locale) {
  return RegistryCacheDir() / "i18n" / (std::string(locale) + ".json");
}

std::filesystem::path RemoteWebStaticDir() {
  std::filesystem::path source = VINPUT_REMOTE_WEB_DIR_SOURCE_PATH;
  std::error_code ec;
  if (std::filesystem::exists(source, ec) && !ec) {
    return source;
  }
  return {VINPUT_REMOTE_WEB_DIR_INSTALL_PATH};
}

} // namespace vinput::path
