#include "common/registry/registry_scripts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <glaze/core/common.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
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

namespace fs = std::filesystem;

class TmpFileGuard {
public:
  explicit TmpFileGuard(fs::path path) : path_(std::move(path)) {}
  ~TmpFileGuard() {
    if (active_ && !path_.empty()) {
      std::error_code ec;
      fs::remove(path_, ec);
    }
  }

  TmpFileGuard(const TmpFileGuard&) = delete;
  TmpFileGuard& operator=(const TmpFileGuard&) = delete;
  TmpFileGuard(TmpFileGuard&&) = delete;
  TmpFileGuard& operator=(TmpFileGuard&&) = delete;

  void Dismiss() noexcept { active_ = false; }

private:
  fs::path path_;
  bool active_ = true;
};

bool IsValidResourceIdBasic(std::string_view id) {
  if (id.empty() || id == "." || id == "..") {
    return false;
  }
  return id.find('/') == std::string_view::npos && id.find('\\') == std::string_view::npos;
}

bool IsValidSegment(std::string_view token) {
  return !token.empty() && token != "." && token != "..";
}

bool IsValidPathComponent(const fs::path& component) {
  const std::string part = component.string();
  if (!IsValidSegment(part)) {
    return false;
  }
  return part.find('/') == std::string::npos && part.find('\\') == std::string::npos;
}

std::vector<std::string> SplitResourceId(std::string_view id) {
  if (!IsValidResourceIdBasic(id)) {
    return {};
  }

  const auto tokens = vinput::str::Split(id, '.');
  if (tokens.size() < 3) {
    return {};
  }

  std::vector<std::string> raw_segments;
  raw_segments.reserve(tokens.size());
  for (const auto& token : tokens) {
    if (!IsValidSegment(token)) {
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
  if (type == "trigger") {
    return vinput::path::ManagedTriggerDir();
  }
  if (type == "receiver") {
    return vinput::path::ManagedReceiverDir();
  }
  return {};
}

bool TypeMatchesKind(std::string_view type, Kind kind) {
  switch (kind) {
  case Kind::kAsrProvider:
    return type == "provider";
  case Kind::kLlmAdapter:
    return type == "adapter";
  case Kind::kTrigger:
    return type == "trigger";
  case Kind::kReceiver:
    return type == "receiver";
  }
  return false;
}

std::string_view KindToManifestString(Kind kind) {
  switch (kind) {
  case Kind::kAsrProvider:
    return "provider";
  case Kind::kLlmAdapter:
    return "adapter";
  case Kind::kTrigger:
    return "trigger";
  case Kind::kReceiver:
    return "receiver";
  }
  return "";
}

std::filesystem::path RegistryCachePathForKind(Kind kind) {
  switch (kind) {
  case Kind::kAsrProvider:
    return vinput::registry::cache::AsrProviderRegistryPath();
  case Kind::kLlmAdapter:
    return vinput::registry::cache::LlmAdapterRegistryPath();
  case Kind::kTrigger:
    return vinput::path::RegistryCacheDir() / "triggers.json";
  case Kind::kReceiver:
    return vinput::path::RegistryCacheDir() / "receivers.json";
  }
  return {};
}

std::vector<EnvSpec> ParseEnvSpecs(const glz::json_t& item) {
  const auto* envs = vinput::json::Find(item, "envs");
  if (envs == nullptr || !envs->is_array()) {
    return {};
  }
  std::vector<EnvSpec> result;
  for (const auto& value : envs->get_array()) {
    if (!value.is_object()) {
      continue;
    }
    EnvSpec env;
    env.name = vinput::json::String(value, "name");
    env.required = vinput::json::Bool(value, "required");
    if (!env.name.empty()) {
      result.push_back(std::move(env));
    }
  }
  return result;
}

std::optional<RegistryEntry> ParseRegistryEntry(const glz::json_t& item) {
  RegistryEntry entry;
  entry.id = vinput::json::String(item, "id");
  entry.short_id = vinput::json::String(item, "short_id");
  entry.version = vinput::json::String(item, "version");
  entry.sha256 = vinput::json::String(item, "sha256");
  entry.vinput_version = vinput::json::String(item, "vinput_version");
  entry.stream = vinput::json::Bool(item, "stream");
  entry.command = vinput::json::String(item, "command");
  entry.readme_url = vinput::json::String(item, "readme_url");
  for (auto& url : vinput::json::Strings(item, "script_urls")) {
    if (!url.empty()) {
      entry.script_urls.push_back(std::move(url));
    }
  }
  entry.envs = ParseEnvSpecs(item);
  if (entry.id.empty() || entry.command.empty() || entry.script_urls.empty()) {
    return std::nullopt;
  }
  return entry;
}

std::vector<RegistryEntry> ParseRegistryJson(const std::string& content, std::string* error) {
  glz::json_t root;
  if (const auto ec = glz::read_json(root, content); bool(ec)) {
    if (error != nullptr) {
      *error = "failed to parse script registry JSON: " + glz::format_error(ec, content);
    }
    return {};
  }
  const auto* items = vinput::json::Find(root, "items");
  if (items == nullptr) {
    if (error != nullptr) {
      *error = "script registry JSON is not an object or is missing array field 'items'";
    }
    return {};
  }
  const auto* item_array = items->get_if<glz::json_t::array_t>();
  if (item_array == nullptr) {
    if (error != nullptr) {
      *error = "script registry JSON is missing array field 'items'";
    }
    return {};
  }

  std::vector<RegistryEntry> entries;
  for (const auto& item : *item_array) {
    if (auto entry = ParseRegistryEntry(item)) {
      entries.push_back(std::move(*entry));
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  return entries;
}

bool EnsureExecutable(const fs::path& path, std::string* error) {
  std::error_code ec;
  fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                  fs::perm_options::add, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to mark script executable: " + ec.message();
    }
    return false;
  }
  return true;
}

void FillDefaultEnvMap(const std::vector<EnvSpec>& envs,
                       std::map<std::string, std::string>* target) {
  if (target == nullptr) {
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
  return provider != nullptr ? std::get_if<CommandAsrProvider>(provider) : nullptr;
}

std::vector<RegistryEntry> FetchRegistryImpl(const CoreConfig* config, Kind kind,
                                             const std::vector<std::string>& urls,
                                             std::string* error, std::string* resolved_registry_url,
                                             std::vector<std::string>* warnings) {
  if (urls.empty()) {
    if (error != nullptr) {
      *error = "no script registry URLs configured";
    }
    return {};
  }

  std::string content;
  vinput::download::Options options;
  options.timeout_seconds = 30;
  options.max_bytes = 4ULL * 1024ULL * 1024ULL;
  vinput::download::Result result;
  if (!vinput::registry::FetchRegistryText(config, urls, RegistryCachePathForKind(kind), options,
                                           &content, &result, error, warnings)) {
    if (resolved_registry_url != nullptr) {
      resolved_registry_url->clear();
    }
    return {};
  }

  if (resolved_registry_url != nullptr) {
    *resolved_registry_url = result.resolved_url;
  }
  return ParseRegistryJson(content, error);
}

bool VerifyDownloadedSha(const fs::path& path, const std::string& expected_sha,
                         std::string* out_sha, std::string* error) {
  auto sha_res = vinput::file::ComputeFileSha256(path);
  if (!sha_res) {
    if (error != nullptr) {
      *error = "failed to compute sha256 for " + path.string() + ": " + sha_res.error();
    }
    return false;
  }
  if (!expected_sha.empty() && *sha_res != expected_sha) {
    if (error != nullptr) {
      *error = "SHA-256 mismatch: expected " + expected_sha + ", got " + *sha_res;
    }
    return false;
  }
  if (out_sha != nullptr) {
    *out_sha = std::move(*sha_res);
  }
  return true;
}

bool PrepareScriptDestination(Kind kind, const RegistryEntry& entry, fs::path* out_path,
                              std::string* error) {
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
    if (error != nullptr) {
      *error = "invalid script id: " + entry.id;
    }
    return false;
  }

  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to create script directory: " + ec.message();
    }
    return false;
  }

  *out_path = path;
  return true;
}

bool PerformScriptDownload(const std::vector<std::string>& urls, const fs::path& tmp_path,
                           const std::atomic<bool>* cancel_token, std::string* resolved_url,
                           std::string* error) {
  vinput::download::Options options;
  options.timeout_seconds = 120;
  options.cancel_token = cancel_token;
  vinput::download::Result result;
  if (!vinput::download::DownloadFile(urls, tmp_path, options, &result)) {
    if (resolved_url != nullptr) {
      resolved_url->clear();
    }
    if (error != nullptr) {
      *error = result.error.empty() ? "failed to download script" : result.error;
    }
    return false;
  }
  if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
    if (resolved_url != nullptr) {
      resolved_url->clear();
    }
    if (error != nullptr) {
      *error = "download cancelled";
    }
    return false;
  }
  if (resolved_url != nullptr) {
    *resolved_url = result.resolved_url;
  }
  return true;
}

bool UpdateCommandAsrProvider(CommandAsrProvider* command_provider, const fs::path& managed_path,
                              const RegistryEntry& entry, const fs::path& script_path,
                              std::string* error) {
  if (command_provider == nullptr || !IsManagedScriptPath(managed_path, command_provider->args)) {
    if (error != nullptr) {
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
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

LlmAdapter* UpsertLlmAdapter(std::vector<LlmAdapter>* adapters, const fs::path& managed_path,
                             const RegistryEntry& entry, const fs::path& script_path,
                             std::string* error) {
  auto it = std::ranges::find_if(
      *adapters, [&entry](const LlmAdapter& adapter) { return adapter.id == entry.id; });
  if (it == adapters->end()) {
    LlmAdapter adapter;
    adapter.id = entry.id;
    adapter.command = entry.command;
    adapter.args = {script_path.string()};
    FillDefaultEnvMap(entry.envs, &adapter.env);
    adapters->push_back(std::move(adapter));
    return &adapters->back();
  }
  if (!IsManagedScriptPath(managed_path, it->args)) {
    if (error != nullptr) {
      *error = "refusing to overwrite user-defined adapter: " + entry.id;
    }
    return nullptr;
  }
  it->command = entry.command;
  it->args = {script_path.string()};
  FillDefaultEnvMap(entry.envs, &it->env);
  return &(*it);
}

UpdateStatus CompareVersionsOrSha(const RegistryEntry& entry, const ResourceVersionInfo& info) {
  if (!entry.version.empty() && !info.installed_version.empty()) {
    if (entry.version != info.installed_version) {
      return UpdateStatus::kUpdateAvailable;
    }
  }

  if (!entry.sha256.empty() && !info.installed_sha256.empty()) {
    return (entry.sha256 != info.installed_sha256) ? UpdateStatus::kUpdateAvailable
                                                   : UpdateStatus::kUpToDate;
  }

  if (!entry.version.empty()) {
    return (entry.version == info.installed_version) ? UpdateStatus::kUpToDate
                                                     : UpdateStatus::kUpdateAvailable;
  }

  return UpdateStatus::kUpToDate;
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
  if (!IsValidResourceIdBasic(type) || type.find('.') != std::string_view::npos) {
    return {};
  }

  std::vector<std::string> segments = {std::string(type)};
  for (const auto& component : relative_path) {
    if (!IsValidPathComponent(component)) {
      return {};
    }
    segments.push_back(component.string());
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
  fs::path path;
  if (!PrepareScriptDestination(kind, entry, &path, error)) {
    return false;
  }

  const fs::path tmp_path =
      path.string() + ".download." +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
  TmpFileGuard guard(tmp_path);

  std::string resolved_url;
  if (!PerformScriptDownload(entry.script_urls, tmp_path, cancel_token, &resolved_url, error)) {
    return false;
  }

  std::string final_sha;
  if (!VerifyDownloadedSha(tmp_path, entry.sha256, &final_sha, error) ||
      !EnsureExecutable(tmp_path, error)) {
    return false;
  }

  if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
    if (error != nullptr) {
      *error = "download cancelled";
    }
    return false;
  }

  std::error_code ec;
  fs::rename(tmp_path, path, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to atomically replace script: " + ec.message();
    }
    return false;
  }
  guard.Dismiss();

  auto manifest = vinput::registry::InstalledManifest::Load();
  manifest.RecordInstalled(entry.id, KindToManifestString(kind), entry.version, final_sha);
  std::string manifest_err;
  manifest.Save({}, &manifest_err);

  if (resolved_script_url != nullptr) {
    *resolved_script_url = std::move(resolved_url);
  }
  if (local_path != nullptr) {
    *local_path = path;
  }
  return true;
}

bool MaterializeAsrProvider(CoreConfig* config, const RegistryEntry& entry,
                            const fs::path& script_path, std::string* error) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "config is null";
    }
    return false;
  }

  const fs::path managed_path = DefaultLocalScriptPath(Kind::kAsrProvider, entry.id);
  if (managed_path.empty()) {
    if (error != nullptr) {
      *error = "invalid ASR provider id: " + entry.id;
    }
    return false;
  }

  auto it = std::ranges::find_if(config->asr.providers, [&entry](const AsrProvider& provider) {
    return AsrProviderId(provider) == entry.id;
  });

  if (it == config->asr.providers.end()) {
    CommandAsrProvider provider;
    provider.id = entry.id;
    provider.timeoutMs = 60000;
    provider.command = entry.command;
    provider.args = {script_path.string()};
    FillDefaultEnvMap(entry.envs, &provider.env);
    config->asr.providers.emplace_back(std::move(provider));
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  return UpdateCommandAsrProvider(GetCommandAsrProvider(&(*it)), managed_path, entry, script_path,
                                  error);
}

bool MaterializeLlmAdapter(CoreConfig* config, const RegistryEntry& entry,
                           const fs::path& script_path, std::string* error,
                           const std::map<std::string, std::string>& env_overrides) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "config is null";
    }
    return false;
  }

  const fs::path managed_path = DefaultLocalScriptPath(Kind::kLlmAdapter, entry.id);
  if (managed_path.empty()) {
    if (error != nullptr) {
      *error = "invalid adapter id: " + entry.id;
    }
    return false;
  }

  auto* target = UpsertLlmAdapter(&config->llm.adapters, managed_path, entry, script_path, error);
  if (target == nullptr) {
    return false;
  }

  for (const auto& [key, val] : env_overrides) {
    if (!key.empty()) {
      target->env[key] = val;
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

namespace {

bool MaterializeClient(std::vector<ClientSpec>* clients, Kind kind, const RegistryEntry& entry,
                       const fs::path& script_path, std::string_view type_label,
                       std::string* error) {
  if (clients == nullptr) {
    if (error != nullptr) {
      *error = "client list is null";
    }
    return false;
  }

  const fs::path managed_path = DefaultLocalScriptPath(kind, entry.id);
  if (managed_path.empty()) {
    if (error != nullptr) {
      *error = "invalid " + std::string(type_label) + " id: " + entry.id;
    }
    return false;
  }

  auto it = std::ranges::find_if(
      *clients, [&entry](const ClientSpec& client) { return client.id == entry.id; });

  if (it == clients->end()) {
    ClientSpec client;
    client.id = entry.id;
    client.command = entry.command;
    client.args = {script_path.string()};
    client.autoStart = false;
    FillDefaultEnvMap(entry.envs, &client.env);
    clients->push_back(std::move(client));
  } else {
    if (!IsManagedScriptPath(managed_path, it->args)) {
      if (error != nullptr) {
        *error = "refusing to overwrite user-defined " + std::string(type_label) + ": " + entry.id;
      }
      return false;
    }
    it->command = entry.command;
    it->args = {script_path.string()};
    FillDefaultEnvMap(entry.envs, &it->env);
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace

bool MaterializeTrigger(CoreConfig* config, const RegistryEntry& entry, const fs::path& script_path,
                        std::string* error) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "config is null";
    }
    return false;
  }
  return MaterializeClient(&config->triggers, Kind::kTrigger, entry, script_path, "trigger", error);
}

bool MaterializeReceiver(CoreConfig* config, const RegistryEntry& entry,
                         const fs::path& script_path, std::string* error) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "config is null";
    }
    return false;
  }
  return MaterializeClient(&config->receivers, Kind::kReceiver, entry, script_path, "receiver",
                           error);
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
    info.installed_sha256 = vinput::file::ComputeFileSha256(script_path).value_or("");
  }

  if (!script_exists) {
    info.status = UpdateStatus::kUpdateAvailable;
    return info;
  }

  info.status = CompareVersionsOrSha(entry, info);
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
