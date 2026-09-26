#include "common/registry/registry_scripts.h"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <system_error>

#include "common/config/core_config_types.h"
#include "common/registry/registry_cache.h"
#include "common/registry/registry_fetch.h"
#include "common/utils/downloader.h"
#include "common/utils/path_utils.h"

namespace vinput::script {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::vector<std::string> SplitResourceId(std::string_view id) {
  if (id.empty() || id == "." || id == ".." || id.find('/') != std::string_view::npos ||
      id.find('\\') != std::string_view::npos) {
    return {};
  }

  std::vector<std::string> raw_segments;
  std::size_t start = 0;
  while (start <= id.size()) {
    const std::size_t dot = id.find('.', start);
    const std::size_t end = dot == std::string_view::npos ? id.size() : dot;
    if (end == start) {
      return {};
    }
    std::string segment(id.substr(start, end - start));
    if (segment.empty() || segment == "." || segment == "..") {
      return {};
    }
    raw_segments.push_back(std::move(segment));
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }

  if (raw_segments.size() < 3) {
    return {};
  }

  const std::string& type = raw_segments[0];
  const std::string& parent = raw_segments[1];
  std::string leaf;
  for (std::size_t i = 2; i < raw_segments.size(); ++i) {
    if (!leaf.empty()) {
      leaf += '.';
    }
    leaf += raw_segments[i];
  }
  if (type.empty() || parent.empty() || leaf.empty()) {
    return {};
  }

  return {type, parent, leaf};
}

std::filesystem::path ManagedBaseDirForType(std::string_view type) {
  if (type == "provider") {
    return vinput::path::ManagedAsrProviderDir();
  }
  if (type == "adapter") {
    return vinput::path::ManagedLlmAdapterDir();
  }
  return {};
}

bool TypeMatchesKind(std::string_view type, Kind kind) {
  switch (kind) {
  case Kind::kAsrProvider:
    return type == "provider";
  case Kind::kLlmAdapter:
    return type == "adapter";
  }
  return false;
}

std::vector<RegistryEntry> ParseRegistryJson(const std::string& content, std::string* error) {
  std::vector<RegistryEntry> entries;
  try {
    const json j = json::parse(content);
    if (!j.is_object()) {
      if (error) {
        *error = "script registry JSON is not an object";
      }
      return {};
    }
    if (!j.contains("items") || !j.at("items").is_array()) {
      if (error) {
        *error = "script registry JSON is missing array field 'items'";
      }
      return {};
    }

    for (const auto& item : j.at("items")) {
      RegistryEntry entry;
      entry.id = item.value("id", "");
      entry.short_id = item.value("short_id", "");
      entry.stream = item.value("stream", false);
      entry.command = item.value("command", "");
      entry.readme_url = item.value("readme_url", "");
      if (item.contains("script_urls") && item.at("script_urls").is_array()) {
        for (const auto& value : item.at("script_urls")) {
          if (value.is_string()) {
            const auto url = value.get<std::string>();
            if (!url.empty()) {
              entry.script_urls.push_back(url);
            }
          }
        }
      }
      if (item.contains("envs") && item.at("envs").is_array()) {
        for (const auto& value : item.at("envs")) {
          if (!value.is_object()) {
            continue;
          }
          EnvSpec env;
          env.name = value.value("name", "");
          env.required = value.value("required", false);
          if (!env.name.empty()) {
            entry.envs.push_back(std::move(env));
          }
        }
      }
      if (!entry.id.empty() && !entry.command.empty() && !entry.script_urls.empty()) {
        entries.push_back(std::move(entry));
      }
    }
  } catch (const std::exception& ex) {
    if (error) {
      *error = std::string("failed to parse script registry JSON: ") + ex.what();
    }
    return {};
  }

  if (error) {
    error->clear();
  }
  return entries;
}

bool EnsureExecutable(const fs::path& path, std::string* error) {
  std::error_code ec;
  fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                  fs::perm_options::add, ec);
  if (ec) {
    if (error) {
      *error = "failed to mark script executable: " + ec.message();
    }
    return false;
  }
  return true;
}

void FillDefaultEnvMap(const std::vector<EnvSpec>& envs,
                       std::map<std::string, std::string>* target) {
  if (!target) {
    return;
  }
  for (const auto& env : envs) {
    target->try_emplace(env.name, "");
  }
}

bool IsManagedScriptPath(const fs::path& expected, const std::vector<std::string>& args) {
  if (args.size() != 1) {
    return false;
  }
  return fs::path(args.front()).lexically_normal() == expected.lexically_normal();
}

CommandAsrProvider* GetCommandAsrProvider(AsrProvider* provider) {
  return provider ? std::get_if<CommandAsrProvider>(provider) : nullptr;
}

std::vector<RegistryEntry> FetchRegistryImpl(const CoreConfig* config, Kind kind,
                                             const std::vector<std::string>& urls,
                                             std::string* error, std::string* resolved_registry_url,
                                             std::vector<std::string>* warnings) {
  if (urls.empty()) {
    if (error) {
      *error = "no script registry URLs configured";
    }
    return {};
  }

  std::string content;
  vinput::download::Options options;
  options.timeout_seconds = 30;
  options.max_bytes = 4 * 1024 * 1024;
  vinput::download::Result result;
  if (!vinput::registry::FetchRegistryText(config, urls,
                                           kind == Kind::kAsrProvider
                                               ? vinput::registry::cache::AsrProviderRegistryPath()
                                               : vinput::registry::cache::LlmAdapterRegistryPath(),
                                           options, &content, &result, error, warnings)) {
    if (resolved_registry_url) {
      resolved_registry_url->clear();
    }
    return {};
  }

  if (resolved_registry_url) {
    *resolved_registry_url = result.resolved_url;
  }
  return ParseRegistryJson(content, error);
}

} // namespace

std::vector<RegistryEntry> FetchRegistry(Kind kind, const std::vector<std::string>& urls,
                                         std::string* error, std::string* resolved_registry_url) {
  return FetchRegistryImpl(nullptr, kind, urls, error, resolved_registry_url, nullptr);
}

std::vector<RegistryEntry> FetchRegistry(const CoreConfig& config, Kind kind,
                                         const std::vector<std::string>& urls, std::string* error,
                                         std::string* resolved_registry_url,
                                         std::vector<std::string>* warnings) {
  return FetchRegistryImpl(&config, kind, urls, error, resolved_registry_url, warnings);
}

std::filesystem::path RelativePathForId(std::string_view id) {
  const auto segments = SplitResourceId(id);
  if (segments.size() != 3) {
    return {};
  }

  fs::path relative_path;
  for (std::size_t i = 1; i < segments.size(); ++i) {
    relative_path /= segments[i];
  }
  return relative_path;
}

std::string IdFromRelativePath(std::string_view type, const std::filesystem::path& relative_path) {
  if (type.empty() || type == "." || type == ".." || type.find('.') != std::string_view::npos ||
      type.find('/') != std::string_view::npos || type.find('\\') != std::string_view::npos) {
    return {};
  }

  std::vector<std::string> segments = {std::string(type)};
  for (const auto& component : relative_path) {
    const std::string part = component.string();
    if (part.empty() || part == "." || part == ".." || part.find('/') != std::string::npos ||
        part.find('\\') != std::string::npos) {
      return {};
    }
    segments.push_back(part);
  }
  if (segments.size() != 3) {
    return {};
  }

  std::string id;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i > 0) {
      id += '.';
    }
    id += segments[i];
  }
  return id;
}

std::filesystem::path DefaultLocalScriptPath(Kind kind, std::string_view id) {
  const auto segments = SplitResourceId(id);
  if (segments.size() != 3) {
    return {};
  }

  if (!TypeMatchesKind(segments.front(), kind)) {
    return {};
  }

  const fs::path base = ManagedBaseDirForType(segments.front());
  if (base.empty()) {
    return {};
  }
  const fs::path relative_path = RelativePathForId(id);
  return base / relative_path;
}

bool DownloadScript(const RegistryEntry& entry, Kind kind, std::filesystem::path* local_path,
                    std::string* error, std::string* resolved_script_url) {
  const fs::path path = DefaultLocalScriptPath(kind, entry.id);
  if (path.empty()) {
    if (error) {
      *error = "invalid script id: " + entry.id;
    }
    return false;
  }

  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error) {
      *error = "failed to create script directory: " + ec.message();
    }
    return false;
  }

  vinput::download::Options options;
  options.timeout_seconds = 120;
  vinput::download::Result result;
  if (!vinput::download::DownloadFile(entry.script_urls, path, options, &result)) {
    if (resolved_script_url) {
      resolved_script_url->clear();
    }
    if (error) {
      *error = result.error.empty() ? "failed to download script" : result.error;
    }
    return false;
  }
  if (!EnsureExecutable(path, error)) {
    return false;
  }
  if (resolved_script_url) {
    *resolved_script_url = result.resolved_url;
  }
  if (local_path) {
    *local_path = path;
  }
  return true;
}

bool MaterializeAsrProvider(CoreConfig* config, const RegistryEntry& entry,
                            const fs::path& script_path, std::string* error) {
  if (!config) {
    if (error) {
      *error = "config is null";
    }
    return false;
  }

  auto it = std::find_if(
      config->asr.providers.begin(), config->asr.providers.end(),
      [&entry](const AsrProvider& provider) { return AsrProviderId(provider) == entry.id; });
  const fs::path managed_path = DefaultLocalScriptPath(Kind::kAsrProvider, entry.id);
  if (managed_path.empty()) {
    if (error) {
      *error = "invalid ASR provider id: " + entry.id;
    }
    return false;
  }

  if (it == config->asr.providers.end()) {
    CommandAsrProvider provider;
    provider.id = entry.id;
    provider.timeoutMs = 60000;
    provider.command = entry.command;
    provider.args = {script_path.string()};
    FillDefaultEnvMap(entry.envs, &provider.env);
    config->asr.providers.push_back(std::move(provider));
  } else {
    auto* command_provider = GetCommandAsrProvider(&(*it));
    if (!command_provider || !IsManagedScriptPath(managed_path, command_provider->args)) {
      if (error) {
        *error = "refusing to overwrite user-defined ASR provider: " + entry.id;
      }
      return false;
    }
    command_provider->command = entry.command;
    command_provider->args = {script_path.string()};
    if (command_provider->timeoutMs <= 0) {
      command_provider->timeoutMs = 60000;
    }
    FillDefaultEnvMap(entry.envs, &command_provider->env);
  }

  if (error) {
    error->clear();
  }
  return true;
}

bool MaterializeLlmAdapter(CoreConfig* config, const RegistryEntry& entry,
                           const fs::path& script_path, std::string* error,
                           const std::map<std::string, std::string>& env_overrides) {
  if (!config) {
    if (error) {
      *error = "config is null";
    }
    return false;
  }

  auto it = std::find_if(config->llm.adapters.begin(), config->llm.adapters.end(),
                         [&entry](const LlmAdapter& adapter) { return adapter.id == entry.id; });
  const fs::path managed_path = DefaultLocalScriptPath(Kind::kLlmAdapter, entry.id);
  if (managed_path.empty()) {
    if (error) {
      *error = "invalid adapter id: " + entry.id;
    }
    return false;
  }

  if (it == config->llm.adapters.end()) {
    LlmAdapter adapter;
    adapter.id = entry.id;
    adapter.command = entry.command;
    adapter.args = {script_path.string()};
    FillDefaultEnvMap(entry.envs, &adapter.env);
    config->llm.adapters.push_back(std::move(adapter));
  } else {
    if (!IsManagedScriptPath(managed_path, it->args)) {
      if (error) {
        *error = "refusing to overwrite user-defined adapter: " + entry.id;
      }
      return false;
    }
    it->command = entry.command;
    it->args = {script_path.string()};
    FillDefaultEnvMap(entry.envs, &it->env);
  }

  // FillDefaultEnvMap only seeds empty defaults, so registry-declared required
  // envs stay empty unless the caller supplies a value.
  for (auto& adapter : config->llm.adapters) {
    if (adapter.id != entry.id) {
      continue;
    }
    for (const auto& override : env_overrides) {
      if (!override.first.empty()) {
        adapter.env[override.first] = override.second;
      }
    }
    break;
  }

  if (error) {
    error->clear();
  }
  return true;
}

} // namespace vinput::script
