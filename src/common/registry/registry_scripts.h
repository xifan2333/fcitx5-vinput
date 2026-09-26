#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct CoreConfig;

namespace vinput::script {

enum class Kind {
  kAsrProvider,
  kLlmAdapter,
};

struct EnvSpec {
  std::string name;
  bool required = false;
};

struct RegistryEntry {
  std::string id;
  std::string short_id;
  bool stream = false;
  std::string command;
  std::vector<std::string> script_urls;
  std::string readme_url;
  std::vector<EnvSpec> envs;
};

std::vector<RegistryEntry> FetchRegistry(Kind kind, const std::vector<std::string>& urls,
                                         std::string* error,
                                         std::string* resolved_registry_url = nullptr);
std::vector<RegistryEntry> FetchRegistry(const CoreConfig& config, Kind kind,
                                         const std::vector<std::string>& urls, std::string* error,
                                         std::string* resolved_registry_url = nullptr,
                                         std::vector<std::string>* warnings = nullptr);
std::filesystem::path RelativePathForId(std::string_view id);
std::string IdFromRelativePath(std::string_view type, const std::filesystem::path& relative_path);
std::filesystem::path DefaultLocalScriptPath(Kind kind, std::string_view id);
bool DownloadScript(const RegistryEntry& entry, Kind kind, std::filesystem::path* local_path,
                    std::string* error, std::string* resolved_script_url = nullptr);
bool MaterializeAsrProvider(CoreConfig* config, const RegistryEntry& entry,
                            const std::filesystem::path& script_path, std::string* error);
bool MaterializeLlmAdapter(CoreConfig* config, const RegistryEntry& entry,
                           const std::filesystem::path& script_path, std::string* error,
                           const std::map<std::string, std::string>& env_overrides = {});

} // namespace vinput::script
