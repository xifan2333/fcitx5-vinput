#pragma once
#include <filesystem>
#include <string_view>

namespace vinput::path {
std::string_view DaemonServiceUnitName();
std::filesystem::path DaemonExecutablePath();
std::filesystem::path DaemonServiceUnitInstallPath();
std::filesystem::path DaemonServiceUnitTemplatePath();
std::filesystem::path ExpandUserPath(std::string_view path);
std::filesystem::path VinputConfigDir();
std::filesystem::path VinputDataDir();
std::filesystem::path DefaultModelBaseDir();
std::filesystem::path CoreConfigPath();
std::filesystem::path FcitxAddonConfigPath();
std::filesystem::path RegistryCacheDir();
std::filesystem::path UserSystemdUnitDir();
std::filesystem::path ManagedResourceDir(std::string_view category);
std::filesystem::path ManagedAsrProviderDir();
std::filesystem::path ManagedLlmAdapterDir();
std::filesystem::path AdapterRuntimeDir();
std::filesystem::path ContextCachePath();
std::filesystem::path ReadNotificationsPath();
std::filesystem::path ProviderModelCachePath();
} // namespace vinput::path
