#include "cli/utils/resource_utils.h"

#include <string>
#include <vector>

#include "common/config/core_config_view.h"

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
  ModelRepository repository(CoreConfigView::ModelBaseDir().string());
  std::string error;
  const auto models = repository.FetchRegistry(config, urls, &error);
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

} // namespace vinput::cli
