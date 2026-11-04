#pragma once

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/asr/model_manager.h"
#include "common/registry/installed_manifest.h"
#include "common/registry/registry_i18n.h"
#include "common/registry/registry_models.h"
#include "common/registry/registry_scripts.h"

#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

struct CoreConfig;

namespace vinput::cli {

struct ResourceDisplayInfo {
  std::string id;
  std::string short_id;
  std::string title;
  std::string description;
  std::string readme_url;
};

using ResourceDisplayMap = std::unordered_map<std::string, ResourceDisplayInfo>;

std::string HumanizeResourceId(const std::string& id, const std::string& short_id);
std::string HumanizeResourceId(const ResourceDisplayMap& display_map, const std::string& id);
std::string FormatTerminalLink(const CliContext& ctx, const std::string& label,
                               const std::string& url);

ResourceDisplayMap BuildModelDisplayMap(const std::vector<RemoteModelEntry>& models,
                                        const vinput::registry::I18nMap& i18n_map);
ResourceDisplayMap BuildScriptDisplayMap(const std::vector<vinput::script::RegistryEntry>& entries,
                                         const vinput::registry::I18nMap& i18n_map);

ResourceDisplayMap FetchModelDisplayMap(const CoreConfig& config);
ResourceDisplayMap FetchScriptDisplayMap(const CoreConfig& config, vinput::script::Kind kind);

std::string ResolveModelSelectorByShortId(const std::string& selector,
                                          const std::vector<RemoteModelEntry>& models,
                                          std::string* error);
std::string ResolveModelSelectorByShortId(const std::string& selector,
                                          const std::vector<ModelSummary>& models,
                                          const ResourceDisplayMap& display_map,
                                          std::string* error);
std::string
ResolveScriptSelectorByShortId(const std::string& selector,
                               const std::vector<vinput::script::RegistryEntry>& entries,
                               const std::string& kind_label, std::string* error);
std::string ResolveInstalledAsrProviderSelector(const CoreConfig& config,
                                                const std::string& selector, std::string* error);
std::string ResolveInstalledLlmAdapterSelector(const CoreConfig& config,
                                               const std::string& selector, std::string* error);

bool ParseEnvOverrides(const std::vector<std::string>& env_overrides,
                       std::map<std::string, std::string>* out_env, Formatter& fmt);

void RenderRegistryScriptList(const std::vector<vinput::script::RegistryEntry>& entries,
                              const ResourceDisplayMap& display_map,
                              const vinput::registry::InstalledManifest& manifest,
                              const std::function<bool(const std::string&)>& is_installed,
                              vinput::script::Kind kind, Formatter& fmt, const CliContext& ctx);

} // namespace vinput::cli
