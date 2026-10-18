#include "common/registry/registry_scripts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>

#include "common/config/core_config_types.h"
#include "common/registry/installed_manifest.h"
#include "common/registry/registry_cache.h"
#include "common/registry/registry_fetch.h"
#include "common/utils/downloader.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/version_utils.h"

namespace vinput::script {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::vector<std::string> SplitResourceId(std::string_view id) {
  if (id.empty() || id == "." || id == ".." || id.find('/') != std::string_view::npos ||
      id.find('\\') != std::string_view::npos) {
    return {};
  }

  const auto tokens = vinput::str::SplitByChar(id, '.');
  if (tokens.size() < 3) {
    return {};
  }

  std::vector<std::string> raw_segments;
  raw_segments.reserve(tokens.size());
  for (const auto& token : tokens) {
    if (token.empty() || token == "." || token == "..") {
      return {};
    }
    raw_segments.emplace_back(token);
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
      entry.version = item.value("version", "");
      entry.sha256 = item.value("sha256", "");
      entry.vinput_version = item.value("vinput_version", "");
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
                    std::string* error, std::string* resolved_script_url,
                    const std::atomic<bool>* cancel_token) {
  if (!vinput::version::Satisfies(vinput::version::CurrentHostVersion(), entry.vinput_version)) {
    if (error != nullptr) {
      *error = "Resource '" + entry.id + "' is incompatible with current vinput host (requires " +
               entry.vinput_version + ", current is " +
               std::string(vinput::version::CurrentHostVersion()) + ")";
    }
    return false;
  }

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

  const fs::path tmp_path =
      path.string() + ".download." +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";

  vinput::download::Options options;
  options.timeout_seconds = 120;
  options.cancel_token = cancel_token;
  vinput::download::Result result;
  if (!vinput::download::DownloadFile(entry.script_urls, tmp_path, options, &result)) {
    if (resolved_script_url != nullptr) {
      resolved_script_url->clear();
    }
    if (error != nullptr) {
      *error = result.error.empty() ? "failed to download script" : result.error;
    }
    std::error_code rm_ec;
    fs::remove(tmp_path, rm_ec);
    return false;
  }

  if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
    std::error_code rm_ec;
    fs::remove(tmp_path, rm_ec);
    if (resolved_script_url != nullptr) {
      resolved_script_url->clear();
    }
    if (error != nullptr) {
      *error = "download cancelled";
    }
    return false;
  }

  std::string actual_sha;
  if (!entry.sha256.empty()) {
    std::string sha_error;
    actual_sha = vinput::file::ComputeFileSha256(tmp_path, &sha_error);
    if (!sha_error.empty() || actual_sha != entry.sha256) {
      std::error_code rm_ec;
      fs::remove(tmp_path, rm_ec);
      if (error != nullptr) {
        *error = !sha_error.empty() ? sha_error
                                    : "SHA-256 mismatch for " + entry.id + ": expected " +
                                          entry.sha256 + ", got " + actual_sha;
      }
      return false;
    }
  }

  if (!EnsureExecutable(tmp_path, error)) {
    std::error_code rm_ec;
    fs::remove(tmp_path, rm_ec);
    return false;
  }

  if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
    std::error_code rm_ec;
    fs::remove(tmp_path, rm_ec);
    if (resolved_script_url != nullptr) {
      resolved_script_url->clear();
    }
    if (error != nullptr) {
      *error = "download cancelled";
    }
    return false;
  }

  fs::rename(tmp_path, path, ec);
  if (ec) {
    std::error_code rm_ec;
    fs::remove(tmp_path, rm_ec);
    if (error != nullptr) {
      *error = "failed to atomically replace script: " + ec.message();
    }
    return false;
  }

  const std::string final_sha =
      actual_sha.empty() ? vinput::file::ComputeFileSha256(path) : actual_sha;
  auto manifest = vinput::registry::InstalledManifest::Load();
  manifest.RecordInstalled(entry.id, kind == Kind::kAsrProvider ? "provider" : "adapter",
                           entry.version, final_sha);
  std::string manifest_err;
  manifest.Save({}, &manifest_err);

  if (resolved_script_url != nullptr) {
    *resolved_script_url = result.resolved_url;
  }
  if (local_path != nullptr) {
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

ResourceVersionInfo CheckResourceVersion(const vinput::registry::InstalledManifest& manifest,
                                         Kind kind, const RegistryEntry& entry,
                                         bool is_installed_in_config) {
  ResourceVersionInfo info;
  info.remote_version = entry.version;
  info.remote_sha256 = entry.sha256;
  info.vinput_version = entry.vinput_version;

  if (!vinput::version::Satisfies(vinput::version::CurrentHostVersion(), entry.vinput_version)) {
    info.status = UpdateStatus::kIncompatible;
    return info;
  }

  if (!is_installed_in_config) {
    info.status = UpdateStatus::kNotInstalled;
    return info;
  }

  const auto* record = manifest.Get(entry.id);
  const auto script_path = DefaultLocalScriptPath(kind, entry.id);
  const bool script_exists = !script_path.empty() && fs::exists(script_path);

  if (record != nullptr) {
    info.installed_version = record->version;
    info.installed_sha256 = record->sha256;
  } else if (script_exists) {
    info.installed_sha256 = vinput::file::ComputeFileSha256(script_path);
  }

  if (!script_exists) {
    info.status = UpdateStatus::kUpdateAvailable;
    return info;
  }

  if (!entry.version.empty() && !info.installed_version.empty()) {
    if (entry.version != info.installed_version) {
      info.status = UpdateStatus::kUpdateAvailable;
      return info;
    }
  }

  if (!entry.sha256.empty() && !info.installed_sha256.empty()) {
    if (entry.sha256 != info.installed_sha256) {
      info.status = UpdateStatus::kUpdateAvailable;
      return info;
    }
    info.status = UpdateStatus::kUpToDate;
    return info;
  }

  if (!entry.version.empty() && entry.version == info.installed_version) {
    info.status = UpdateStatus::kUpToDate;
    return info;
  }

  if (!entry.version.empty() && info.installed_version.empty()) {
    info.status = UpdateStatus::kUpdateAvailable;
    return info;
  }

  info.status = UpdateStatus::kUpToDate;
  return info;
}

bool UnregisterInstalledScript(std::string_view id) {
  auto manifest = vinput::registry::InstalledManifest::Load();
  if (manifest.Remove(id)) {
    return manifest.Save();
  }
  return true;
}

} // namespace vinput::script
