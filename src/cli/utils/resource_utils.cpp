#include "cli/utils/resource_utils.h"

#include <algorithm>
#include <filesystem>
#include <fmt/printf.h>
#include <functional>
#include <glaze/json/json_t.hpp>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/asr/model_manager.h"
#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/i18n.h"
#include "common/registry/installed_manifest.h"
#include "common/registry/registry_i18n.h"
#include "common/registry/registry_models.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/string_utils.h"

#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli {

namespace {

template <typename T, typename GetId, typename GetShortId>
std::string ResolveSelector(const std::string& selector, const std::vector<T>& entries,
                            const std::string& kind_label, GetId get_id, GetShortId get_short_id,
                            std::string* error) {
  for (const auto& entry : entries) {
    if (get_short_id(entry) == selector || get_id(entry) == selector) {
      return get_id(entry);
    }
  }

  if (error) {
    *error = kind_label + " not found: " + selector;
  }
  return {};
}

vinput::registry::I18nMap FetchRegistryI18nMap(const CoreConfig& config) {
  std::string error;
  return vinput::registry::FetchMergedI18nMap(config, vinput::registry::DetectPreferredLocale(),
                                              &error);
}

} // namespace

std::string HumanizeResourceId(const std::string& id, const std::string& short_id) {
  return short_id.empty() ? id : short_id;
}

std::string HumanizeResourceId(const ResourceDisplayMap& display_map, const std::string& id) {
  const auto it = display_map.find(id);
  if (it == display_map.end()) {
    return id;
  }
  return HumanizeResourceId(it->second.id, it->second.short_id);
}

std::string FormatTerminalLink(const CliContext& ctx, const std::string& label,
                               const std::string& url) {
  if (label.empty()) {
    return {};
  }
  if (url.empty() || !ctx.is_tty) {
    return label;
  }
  return "\033]8;;" + url + "\a" + label + "\033]8;;\a";
}

ResourceDisplayMap BuildModelDisplayMap(const std::vector<RemoteModelEntry>& models,
                                        const vinput::registry::I18nMap& i18n_map) {
  ResourceDisplayMap display_map;
  for (const auto& model : models) {
    display_map.emplace(
        model.id,
        ResourceDisplayInfo{
            .id = model.id,
            .short_id = model.short_id,
            .title = vinput::registry::LookupI18n(i18n_map, model.id + ".title",
                                                  HumanizeResourceId(model.id, model.short_id)),
            .description = vinput::registry::LookupI18n(i18n_map, model.id + ".description", ""),
            .readme_url = "",
        });
  }
  return display_map;
}

ResourceDisplayMap BuildScriptDisplayMap(const std::vector<vinput::script::RegistryEntry>& entries,
                                         const vinput::registry::I18nMap& i18n_map) {
  ResourceDisplayMap display_map;
  for (const auto& entry : entries) {
    display_map.emplace(
        entry.id,
        ResourceDisplayInfo{
            .id = entry.id,
            .short_id = entry.short_id,
            .title = vinput::registry::LookupI18n(i18n_map, entry.id + ".title",
                                                  HumanizeResourceId(entry.id, entry.short_id)),
            .description = vinput::registry::LookupI18n(i18n_map, entry.id + ".description", ""),
            .readme_url = entry.readme_url,
        });
  }
  return display_map;
}

ResourceDisplayMap FetchModelDisplayMap(const CoreConfig& config) {
  const CoreConfigView view(config);
  const auto urls = view.ModelRegistryUrls();
  if (urls.empty()) {
    return {};
  }
  std::string error;
  const auto models = ModelRepository::FetchRegistry(config, urls, &error);
  if (!error.empty()) {
    return {};
  }
  return BuildModelDisplayMap(models, FetchRegistryI18nMap(config));
}

ResourceDisplayMap FetchScriptDisplayMap(const CoreConfig& config, vinput::script::Kind kind) {
  const CoreConfigView view(config);
  const auto urls = kind == vinput::script::Kind::kAsrProvider ? view.AsrProviderRegistryUrls()
                                                               : view.LlmAdapterRegistryUrls();
  if (urls.empty()) {
    return {};
  }
  std::string error;
  const auto entries = vinput::script::FetchRegistry(config, kind, urls, &error);
  if (!error.empty()) {
    return {};
  }
  return BuildScriptDisplayMap(entries, FetchRegistryI18nMap(config));
}

std::string ResolveModelSelectorByShortId(const std::string& selector,
                                          const std::vector<RemoteModelEntry>& models,
                                          std::string* error) {
  return ResolveSelector(
      selector, models, "model",
      [](const RemoteModelEntry& model) -> const std::string& { return model.id; },
      [](const RemoteModelEntry& model) -> const std::string& { return model.short_id; }, error);
}

std::string ResolveModelSelectorByShortId(const std::string& selector,
                                          const std::vector<ModelSummary>& models,
                                          const ResourceDisplayMap& display_map,
                                          std::string* error) {
  return ResolveSelector(
      selector, models, "installed model",
      [](const ModelSummary& model) -> const std::string& { return model.id; },
      [&display_map](const ModelSummary& model) {
        const auto it = display_map.find(model.id);
        if (it == display_map.end()) {
          return model.id;
        }
        return it->second.short_id.empty() ? model.id : it->second.short_id;
      },
      error);
}

std::string
ResolveScriptSelectorByShortId(const std::string& selector,
                               const std::vector<vinput::script::RegistryEntry>& entries,
                               const std::string& kind_label, std::string* error) {
  return ResolveSelector(
      selector, entries, kind_label,
      [](const vinput::script::RegistryEntry& entry) -> const std::string& { return entry.id; },
      [](const vinput::script::RegistryEntry& entry) -> const std::string& {
        return entry.short_id;
      },
      error);
}

std::string ResolveInstalledAsrProviderSelector(const CoreConfig& config,
                                                const std::string& selector, std::string* error) {
  std::vector<const AsrProvider*> providers;
  providers.reserve(config.asr.providers.size());
  for (const auto& provider : config.asr.providers) {
    providers.push_back(&provider);
  }
  const auto display_map = FetchScriptDisplayMap(config, vinput::script::Kind::kAsrProvider);
  return ResolveSelector(
      selector, providers, "ASR provider",
      [](const AsrProvider* provider) -> const std::string& { return AsrProviderId(*provider); },
      [&display_map](const AsrProvider* provider) {
        const std::string& id = AsrProviderId(*provider);
        const auto it = display_map.find(id);
        if (it == display_map.end() || it->second.short_id.empty()) {
          return id;
        }
        return it->second.short_id;
      },
      error);
}

std::string ResolveInstalledLlmAdapterSelector(const CoreConfig& config,
                                               const std::string& selector, std::string* error) {
  std::vector<const LlmAdapter*> adapters;
  adapters.reserve(config.llm.adapters.size());
  for (const auto& adapter : config.llm.adapters) {
    adapters.push_back(&adapter);
  }
  const auto display_map = FetchScriptDisplayMap(config, vinput::script::Kind::kLlmAdapter);
  return ResolveSelector(
      selector, adapters, "LLM adapter",
      [](const LlmAdapter* adapter) -> const std::string& { return adapter->id; },
      [&display_map](const LlmAdapter* adapter) {
        const auto it = display_map.find(adapter->id);
        if (it == display_map.end() || it->second.short_id.empty()) {
          return adapter->id;
        }
        return it->second.short_id;
      },
      error);
}

bool ParseEnvOverrides(const std::vector<std::string>& env_overrides,
                       std::map<std::string, std::string>* out_env, Formatter& fmt) {
  if (out_env == nullptr) {
    return false;
  }
  out_env->clear();
  for (const auto& item : env_overrides) {
    const auto pair = vinput::str::SplitOnce(item, '=');
    if (!pair || pair->first.empty()) {
      // A malformed value may be a token pasted without its key, so never echo it back.
      fmt.PrintError(_("Invalid --env: expected KEY=VALUE."));
      return false;
    }
    (*out_env)[std::string(pair->first)] = std::string(pair->second);
  }
  return true;
}

namespace {

std::string FormatScriptStatusJson(vinput::script::UpdateStatus status, bool installed) {
  if (status == vinput::script::UpdateStatus::kIncompatible) {
    return "incompatible";
  }
  if (installed) {
    return status == vinput::script::UpdateStatus::kUpdateAvailable ? "update_available"
                                                                    : "installed";
  }
  return "available";
}

std::string FormatScriptStatusTable(const vinput::script::ResourceVersionInfo& ver_info,
                                    bool installed) {
  if (ver_info.status == vinput::script::UpdateStatus::kIncompatible) {
    return ::fmt::sprintf(_("incompatible (requires vinput %s)"), ver_info.vinput_version);
  }
  if (!installed) {
    return _("available");
  }
  if (ver_info.status == vinput::script::UpdateStatus::kUpdateAvailable) {
    if (!ver_info.installed_version.empty() && !ver_info.remote_version.empty()) {
      return ::fmt::sprintf(_("update available (%s -> %s)"), ver_info.installed_version,
                            ver_info.remote_version);
    }
    return _("update available");
  }
  if (!ver_info.installed_version.empty()) {
    return ::fmt::sprintf(_("installed (%s)"), ver_info.installed_version);
  }
  return _("installed");
}

void RenderRegistryScriptListJson(const std::vector<vinput::script::RegistryEntry>& entries,
                                  const ResourceDisplayMap& display_map,
                                  const vinput::registry::InstalledManifest& manifest,
                                  const std::function<bool(const std::string&)>& is_installed,
                                  vinput::script::Kind kind, Formatter& fmt) {
  const bool is_asr = kind == vinput::script::Kind::kAsrProvider;
  glz::json_t::array_t arr;
  arr.reserve(entries.size());
  for (const auto& entry : entries) {
    glz::json_t::array_t envs;
    envs.reserve(entry.envs.size());
    for (const auto& env : entry.envs) {
      envs.emplace_back(glz::json_t::object_t{{"name", env.name}, {"required", env.required}});
    }
    const bool installed = is_installed(entry.id);
    const auto ver_info = vinput::script::CheckResourceVersion(manifest, kind, entry, installed);

    glz::json_t json_entry = {
        {"id", HumanizeResourceId(entry.id, entry.short_id)},
        {"machine_id", entry.id},
        {"title", display_map.at(entry.id).title},
        {"description", display_map.at(entry.id).description},
        {"readme_url", entry.readme_url},
        {"envs", glz::json_t(std::move(envs))},
        {"status", FormatScriptStatusJson(ver_info.status, installed)},
        {"installed_version", ver_info.installed_version},
        {"remote_version", ver_info.remote_version},
        {"vinput_version", ver_info.vinput_version},
    };
    if (is_asr) {
      json_entry["stream"] = entry.stream;
    }
    arr.emplace_back(std::move(json_entry));
  }
  fmt.PrintJson(glz::json_t(std::move(arr)));
}

void RenderRegistryScriptListTable(const std::vector<vinput::script::RegistryEntry>& entries,
                                   const ResourceDisplayMap& display_map,
                                   const vinput::registry::InstalledManifest& manifest,
                                   const std::function<bool(const std::string&)>& is_installed,
                                   vinput::script::Kind kind, Formatter& fmt,
                                   const CliContext& ctx) {
  const bool is_asr = kind == vinput::script::Kind::kAsrProvider;
  const std::vector<std::string> headers =
      is_asr ? std::vector<std::string>{_("ID"), _("TITLE"), _("MODE"), _("STATUS"), _("README")}
             : std::vector<std::string>{_("ID"), _("TITLE"), _("STATUS"), _("README")};

  std::vector<std::vector<std::string>> rows;
  rows.reserve(entries.size());
  for (const auto& entry : entries) {
    const bool installed = is_installed(entry.id);
    const auto ver_info = vinput::script::CheckResourceVersion(manifest, kind, entry, installed);
    const std::string status_text = FormatScriptStatusTable(ver_info, installed);

    if (is_asr) {
      rows.push_back({HumanizeResourceId(entry.id, entry.short_id), display_map.at(entry.id).title,
                      entry.stream ? _("stream") : _("batch"), status_text,
                      FormatTerminalLink(ctx, _("Open README"), entry.readme_url)});
    } else {
      rows.push_back({HumanizeResourceId(entry.id, entry.short_id), display_map.at(entry.id).title,
                      status_text, FormatTerminalLink(ctx, _("Open README"), entry.readme_url)});
    }
  }
  fmt.PrintTable(headers, rows);
}

} // namespace

void RenderRegistryScriptList(const std::vector<vinput::script::RegistryEntry>& entries,
                              const ResourceDisplayMap& display_map,
                              const vinput::registry::InstalledManifest& manifest,
                              const std::function<bool(const std::string&)>& is_installed,
                              vinput::script::Kind kind, Formatter& fmt, const CliContext& ctx) {
  if (ctx.json_output) {
    RenderRegistryScriptListJson(entries, display_map, manifest, is_installed, kind, fmt);
  } else {
    RenderRegistryScriptListTable(entries, display_map, manifest, is_installed, kind, fmt, ctx);
  }
}

std::optional<std::vector<vinput::script::RegistryEntry>>
FetchScriptRegistryEntries(const CoreConfig& config, vinput::script::Kind kind, Formatter& fmt) {
  const CoreConfigView view(config);
  const auto registry_urls = kind == vinput::script::Kind::kAsrProvider
                                 ? view.AsrProviderRegistryUrls()
                                 : view.LlmAdapterRegistryUrls();
  if (registry_urls.empty()) {
    if (kind == vinput::script::Kind::kAsrProvider) {
      fmt.PrintError(_("No ASR provider registry base URLs configured. Edit config.json and set "
                       "registry.base_urls."));
    } else {
      fmt.PrintError(_("No LLM adapter registry base URLs configured. Edit config.json and set "
                       "registry.base_urls."));
    }
    return std::nullopt;
  }

  std::string error;
  auto entries = vinput::script::FetchRegistry(config, kind, registry_urls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return std::nullopt;
  }
  return entries;
}

const vinput::script::RegistryEntry*
FindRegistryScriptEntry(const std::vector<vinput::script::RegistryEntry>& entries,
                        vinput::script::Kind kind, const std::string& selector, Formatter& fmt) {
  const std::string kind_label =
      kind == vinput::script::Kind::kAsrProvider ? "ASR provider" : "LLM adapter";
  std::string error;
  const std::string id = ResolveScriptSelectorByShortId(selector, entries, kind_label, &error);
  if (id.empty()) {
    fmt.PrintError(error);
    return nullptr;
  }

  const auto it = std::ranges::find_if(
      entries, [&id](const vinput::script::RegistryEntry& entry) { return entry.id == id; });
  if (it == entries.end()) {
    if (kind == vinput::script::Kind::kAsrProvider) {
      fmt.PrintError(::fmt::sprintf(_("ASR provider '%s' not found in registry."), id));
    } else {
      fmt.PrintError(::fmt::sprintf(_("Adapter '%s' not found in registry."), id));
    }
    return nullptr;
  }
  return &(*it);
}

std::optional<std::filesystem::path>
DownloadRegistryScript(const vinput::script::RegistryEntry& entry, vinput::script::Kind kind,
                       Formatter& fmt) {
  std::filesystem::path script_path;
  std::string error;
  if (!vinput::script::DownloadScript(entry, kind, &script_path, &error)) {
    fmt.PrintError(error);
    return std::nullopt;
  }
  return script_path;
}

int RunRegistryScriptList(const CoreConfig& config, vinput::script::Kind kind,
                          const std::function<bool(const std::string&)>& is_installed,
                          Formatter& fmt, const CliContext& ctx) {
  const auto entries_opt = FetchScriptRegistryEntries(config, kind, fmt);
  if (!entries_opt) {
    return 1;
  }
  const auto& entries = *entries_opt;

  const auto locale = vinput::registry::DetectPreferredLocale();
  const auto i18nMap = vinput::registry::FetchMergedI18nMap(config, locale);
  const auto display_map = BuildScriptDisplayMap(entries, i18nMap);
  const auto manifest = vinput::registry::InstalledManifest::Load();

  RenderRegistryScriptList(entries, display_map, manifest, is_installed, kind, fmt, ctx);
  return 0;
}

bool UpdateRegistryScript(
    const vinput::registry::InstalledManifest& manifest, const vinput::script::RegistryEntry& entry,
    vinput::script::Kind kind, bool force,
    const std::function<bool(const std::filesystem::path&, std::string*)>& materialize,
    Formatter& fmt) {
  const auto version_info = vinput::script::CheckResourceVersion(manifest, kind, entry, true);
  const bool is_asr = kind == vinput::script::Kind::kAsrProvider;
  if (!force && version_info.status == vinput::script::UpdateStatus::kUpToDate) {
    fmt.PrintInfo(is_asr
                      ? ::fmt::sprintf(_("Provider '%s' is already up to date."), entry.short_id)
                      : ::fmt::sprintf(_("Adapter '%s' is already up to date."), entry.short_id));
    return true;
  }

  std::filesystem::path script_path;
  std::string error;
  if (!vinput::script::DownloadScript(entry, kind, &script_path, &error)) {
    fmt.PrintError(::fmt::sprintf(_("Failed to update '%s': %s"), entry.short_id, error));
    return false;
  }
  if (!materialize(script_path, &error)) {
    fmt.PrintError(::fmt::sprintf(_("Failed to update '%s': %s"), entry.short_id, error));
    return false;
  }

  if (version_info.installed_version.empty() || version_info.remote_version.empty()) {
    fmt.PrintSuccess(is_asr ? ::fmt::sprintf(_("ASR provider '%s' updated."), entry.short_id)
                            : ::fmt::sprintf(_("Adapter '%s' updated."), entry.short_id));
    return true;
  }

  fmt.PrintSuccess(
      is_asr ? ::fmt::sprintf(_("ASR provider '%s' updated: %s -> %s"), entry.short_id,
                              version_info.installed_version, version_info.remote_version)
             : ::fmt::sprintf(_("Adapter '%s' updated: %s -> %s"), entry.short_id,
                              version_info.installed_version, version_info.remote_version));
  return true;
}

} // namespace vinput::cli
