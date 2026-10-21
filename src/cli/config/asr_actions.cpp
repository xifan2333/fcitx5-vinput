#include "cli/config/asr_actions.h"

#include <algorithm>
#include <filesystem>
#include <glaze/json/json_t.hpp>
#include <system_error>
#include <utility>
#include <vector>

#include "common/config/core_config.h"
#include "common/dbus/asr_backend_state_utils.h"
#include "common/i18n.h"
#include "common/json/json_access.h"
#include "common/registry/installed_manifest.h"
#include "common/registry/registry_i18n.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/string_utils.h"

#include "config.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include "common/asr/model_manager.h"
#include "common/registry/registry_models.h"
#include "common/utils/download_progress.h"
#endif

#include <cstdint>
#include <cstdio>
#include <string>
#include <variant>

#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"

#include "cli/runtime/dbus_client.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/cli_helpers.h"
#include "cli/utils/editor_utils.h"
#include "cli/utils/formatter.h"
#include "cli/utils/resource_utils.h"

namespace {

bool IsCommandProvider(const AsrProvider& provider) {
  return std::holds_alternative<CommandAsrProvider>(provider);
}

void ReportAsrRuntimeReloadStatus(const CoreConfig& config, Formatter& fmt,
                                  vinput::cli::DbusClient& dbus) {
  vinput::dbus::AsrBackendState state;
  std::string error;
  if (!dbus.GetAsrBackendState(&state, &error)) {
    if (!error.empty()) {
      fmt.PrintWarning(vinput::str::FmtStr(
          _("Config saved, but failed to confirm ASR runtime state: %s"), error));
    }
    return;
  }

  switch (vinput::dbus::ClassifyRequestedAsrBackend(state, config.asr.activeProvider,
                                                    ResolvePreferredLocalModel(config))) {
  case vinput::dbus::RequestedAsrBackendStatus::kReloadInProgress:
    fmt.PrintInfo(_("Config saved and ASR backend reload is in progress."));
    break;
  case vinput::dbus::RequestedAsrBackendStatus::kApplied:
    fmt.PrintInfo(_("Config saved and ASR runtime applied the requested backend."));
    break;
  case vinput::dbus::RequestedAsrBackendStatus::kFailedStillUsingPrevious:
    fmt.PrintWarning(_("Config saved, but ASR runtime is still using the previous backend."));
    break;
  case vinput::dbus::RequestedAsrBackendStatus::kFailedNoUsableBackend:
    fmt.PrintWarning(_("Config saved, but no usable ASR backend is active."));
    break;
  case vinput::dbus::RequestedAsrBackendStatus::kConfigSaved:
  case vinput::dbus::RequestedAsrBackendStatus::kUnknown:
    fmt.PrintInfo(_("Config saved. ASR runtime state is updating."));
    break;
  }
}

bool SaveAsrConfigAndReload(const CoreConfig& config, Formatter& fmt) {
  if (!SaveConfigOrFail(config, fmt)) {
    return false;
  }

  vinput::cli::DbusClient dbus;
  std::string error;
  if (!dbus.IsDaemonRunning(&error)) {
    if (!error.empty()) {
      fmt.PrintWarning(
          vinput::str::FmtStr(_("Config saved, but ASR backend reload was skipped: %s"), error));
    }
    return true;
  }
  if (!dbus.ReloadAsrBackend(&error)) {
    fmt.PrintWarning(
        vinput::str::FmtStr(_("Config saved, but failed to reload ASR backend: %s"), error));
    return true;
  }
  ReportAsrRuntimeReloadStatus(config, fmt, dbus);
  return true;
}

#if VINPUT_ENABLE_LOCAL_ASR
const LocalAsrProvider* PreferredLocalProvider(const CoreConfig& config) {
  const AsrProvider* provider = ResolvePreferredLocalAsrProvider(config);
  if (provider == nullptr) {
    return nullptr;
  }
  return std::get_if<LocalAsrProvider>(provider);
}

LocalAsrProvider* PreferredLocalProvider(CoreConfig* config) {
  if (config == nullptr) {
    return nullptr;
  }
  const AsrProvider* provider = ResolvePreferredLocalAsrProvider(*config);
  if (provider == nullptr) {
    return nullptr;
  }
  const std::string providerId = AsrProviderId(*provider);
  for (auto& candidate : config->asr.providers) {
    if (AsrProviderId(candidate) == providerId) {
      return std::get_if<LocalAsrProvider>(&candidate);
    }
  }
  return nullptr;
}
#endif

std::filesystem::path ResolveExistingFilePath(const std::string& candidate) {
  if (candidate.empty()) {
    return {};
  }

  std::filesystem::path path = vinput::path::ExpandUserPath(candidate);
  if (path.empty()) {
    return {};
  }
  if (path.is_relative()) {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) {
      return {};
    }
    path = cwd / path;
  }

  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return {};
  }
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return {};
  }
  return path;
}

std::filesystem::path ResolveEditableScriptPath(const AsrProvider& provider) {
  if (!IsCommandProvider(provider)) {
    return {};
  }

  const auto& commandProvider = std::get<CommandAsrProvider>(provider);
  if (commandProvider.command.find('/') != std::string::npos ||
      commandProvider.command.starts_with('.') || commandProvider.command.starts_with('~')) {
    if (auto path = ResolveExistingFilePath(commandProvider.command); !path.empty()) {
      return path;
    }
  }

  for (const auto& arg : commandProvider.args) {
    if (auto path = ResolveExistingFilePath(arg); !path.empty()) {
      return path;
    }
  }

  return {};
}

} // namespace

int RunAsrConfigList(Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const auto display_map =
      vinput::cli::FetchScriptDisplayMap(config, vinput::script::Kind::kAsrProvider);
#if VINPUT_ENABLE_LOCAL_ASR
  const auto model_display_map = vinput::cli::FetchModelDisplayMap(config);
#endif

  if (ctx.json_output) {
    glz::json_t::array_t providers;
    for (const auto& provider : config.asr.providers) {
#if !VINPUT_ENABLE_LOCAL_ASR
      if (std::holds_alternative<LocalAsrProvider>(provider)) {
        continue;
      }
#endif
      const std::string machine_id = AsrProviderId(provider);
      glz::json_t entry = {
          {"id", vinput::cli::HumanizeResourceId(display_map, machine_id)},
          {"machine_id", machine_id},
          {"type", std::string(AsrProviderType(provider))},
          {"active", machine_id == config.asr.activeProvider},
          {"timeout_ms", AsrProviderTimeoutMs(provider)},
      };
      const auto display_it = display_map.find(machine_id);
      if (display_it != display_map.end()) {
        if (!display_it->second.title.empty()) {
          entry["title"] = display_it->second.title;
        }
        if (!display_it->second.description.empty()) {
          entry["description"] = display_it->second.description;
        }
        if (!display_it->second.readme_url.empty()) {
          entry["readme_url"] = display_it->second.readme_url;
        }
      }

#if VINPUT_ENABLE_LOCAL_ASR
      if (const auto* local = std::get_if<LocalAsrProvider>(&provider)) {
        entry["model"] = vinput::cli::HumanizeResourceId(model_display_map, local->model);
        entry["hotwords_file"] = local->hotwordsFile;
      } else
#endif
          if (const auto* command = std::get_if<CommandAsrProvider>(&provider)) {
        entry["command"] = command->command;
        entry["args"] = vinput::json::FromStrings(command->args);
        entry["env"] = vinput::json::FromStringMap(command->env);
      }
      providers.emplace_back(std::move(entry));
    }
    fmt.PrintJson(glz::json_t(std::move(providers)));
    return 0;
  }

  const std::vector<std::string> headers = {_("ID"),     _("TITLE"), _("TYPE"),
                                            _("ACTIVE"), _("MODEL"), _("README")};
  std::vector<std::vector<std::string>> rows;
  for (const auto& provider : config.asr.providers) {
#if !VINPUT_ENABLE_LOCAL_ASR
    if (std::holds_alternative<LocalAsrProvider>(provider)) {
      continue;
    }
#endif
    const std::string id = AsrProviderId(provider);
    const auto display_it = display_map.find(id);
    const std::string type = std::string(AsrProviderType(provider));
    const std::string active = id == config.asr.activeProvider ? _("yes") : _("no");
    std::string model = "-";
#if VINPUT_ENABLE_LOCAL_ASR
    if (const auto* local = std::get_if<LocalAsrProvider>(&provider)) {
      model = local->model.empty()
                  ? _("(not set)")
                  : vinput::cli::HumanizeResourceId(model_display_map, local->model);
    }
#endif
    rows.push_back({vinput::cli::HumanizeResourceId(display_map, id),
                    display_it == display_map.end() ? "" : display_it->second.title, type, active,
                    model,
                    display_it == display_map.end()
                        ? ""
                        : vinput::cli::FormatTerminalLink(ctx, _("Open README"),
                                                          display_it->second.readme_url)});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunAsrConfigRemove(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledAsrProviderSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  auto& providers = config.asr.providers;
  auto it = std::ranges::find_if(providers, [&resolved_id](const AsrProvider& provider) {
    return AsrProviderId(provider) == resolved_id;
  });
  if (it == providers.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("ASR provider '%s' not found."), id));
    return 1;
  }

  if (std::holds_alternative<LocalAsrProvider>(*it)) {
    fmt.PrintError(
        _("The local ASR provider cannot be removed. It is required for model management."));
    return 1;
  }

  providers.erase(it);
  if (config.asr.activeProvider == resolved_id) {
    config.asr.activeProvider.clear();
  }

  const auto managed_path =
      vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kAsrProvider, resolved_id);
  if (!managed_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(managed_path, ec);
  }
  vinput::script::UnregisterInstalledScript(resolved_id);

  if (!SaveAsrConfigAndReload(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("ASR provider '%s' removed."), id));
  return 0;
}

int RunAsrConfigUse(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledAsrProviderSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

#if !VINPUT_ENABLE_LOCAL_ASR
  const auto* target_provider = ResolveAsrProvider(config, resolved_id);
  if (target_provider && std::holds_alternative<LocalAsrProvider>(*target_provider)) {
    fmt.PrintError(vinput::str::FmtStr(
        _("ASR provider '%s' is a local provider, which is not supported in this Lite build."),
        resolved_id));
    return 1;
  }
#endif

  config.asr.activeProvider = resolved_id;
  if (!SaveAsrConfigAndReload(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("Configured ASR provider set to '%s'."), id));
  return 0;
}

int RunAsrConfigListProviders(bool available, Formatter& fmt, const CliContext& ctx) {
  if (!available) {
    return RunAsrConfigList(fmt, ctx);
  }

  CoreConfig config = LoadCoreConfig();
  const auto registryUrls = ResolveAsrProviderRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(_("No ASR provider registry base URLs configured. Edit config.json and set "
                     "registry.base_urls."));
    return 1;
  }

  std::string error;
  const auto entries = vinput::script::FetchRegistry(config, vinput::script::Kind::kAsrProvider,
                                                     registryUrls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto locale = vinput::registry::DetectPreferredLocale();
  const auto i18nMap = vinput::registry::FetchMergedI18nMap(config, locale);
  const auto display_map = vinput::cli::BuildScriptDisplayMap(entries, i18nMap);
  const auto manifest = vinput::registry::InstalledManifest::Load();

  auto isInstalled = [&config](const std::string& id) {
    return ResolveAsrProvider(config, id) != nullptr;
  };

  if (ctx.json_output) {
    glz::json_t::array_t arr;
    for (const auto& entry : entries) {
      glz::json_t::array_t envs;
      for (const auto& env : entry.envs) {
        glz::json_t json_entry = {{"name", env.name}, {"required", env.required}};
        envs.emplace_back(std::move(json_entry));
      }
      const bool installed = isInstalled(entry.id);
      const auto ver_info = vinput::script::CheckResourceVersion(
          manifest, vinput::script::Kind::kAsrProvider, entry, installed);
      std::string status_str = "available";
      if (ver_info.status == vinput::script::UpdateStatus::kIncompatible) {
        status_str = "incompatible";
      } else if (installed) {
        status_str = ver_info.status == vinput::script::UpdateStatus::kUpdateAvailable
                         ? "update_available"
                         : "installed";
      }

      glz::json_t json_entry = {
          {"id", vinput::cli::HumanizeResourceId(entry.id, entry.short_id)},
          {"machine_id", entry.id},
          {"title", display_map.at(entry.id).title},
          {"description", display_map.at(entry.id).description},
          {"readme_url", entry.readme_url},
          {"stream", entry.stream},
          {"envs", glz::json_t(std::move(envs))},
          {"status", status_str},
          {"installed_version", ver_info.installed_version},
          {"remote_version", ver_info.remote_version},
          {"vinput_version", ver_info.vinput_version},
      };
      arr.emplace_back(std::move(json_entry));
    }
    fmt.PrintJson(glz::json_t(std::move(arr)));
    return 0;
  }

  const std::vector<std::string> headers = {_("ID"), _("TITLE"), _("MODE"), _("STATUS"),
                                            _("README")};
  std::vector<std::vector<std::string>> rows;
  for (const auto& entry : entries) {
    const bool installed = isInstalled(entry.id);
    const auto ver_info = vinput::script::CheckResourceVersion(
        manifest, vinput::script::Kind::kAsrProvider, entry, installed);
    std::string status_text = _("available");
    if (ver_info.status == vinput::script::UpdateStatus::kIncompatible) {
      status_text =
          vinput::str::FmtStr(_("incompatible (requires vinput %s)"), ver_info.vinput_version);
    } else if (installed) {
      if (ver_info.status == vinput::script::UpdateStatus::kUpdateAvailable) {
        if (!ver_info.installed_version.empty() && !ver_info.remote_version.empty()) {
          status_text = vinput::str::FmtStr(_("update available (%s -> %s)"),
                                            ver_info.installed_version, ver_info.remote_version);
        } else {
          status_text = _("update available");
        }
      } else if (!ver_info.installed_version.empty()) {
        status_text = vinput::str::FmtStr(_("installed (%s)"), ver_info.installed_version);
      } else {
        status_text = _("installed");
      }
    }

    rows.push_back({vinput::cli::HumanizeResourceId(entry.id, entry.short_id),
                    display_map.at(entry.id).title, entry.stream ? _("stream") : _("batch"),
                    status_text,
                    vinput::cli::FormatTerminalLink(ctx, _("Open README"), entry.readme_url)});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunAsrConfigInstallProvider(const std::string& selector, Formatter& fmt,
                                const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const auto registryUrls = ResolveAsrProviderRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(_("No ASR provider registry base URLs configured. Edit config.json and set "
                     "registry.base_urls."));
    return 1;
  }

  std::string error;
  const auto entries = vinput::script::FetchRegistry(config, vinput::script::Kind::kAsrProvider,
                                                     registryUrls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const std::string id =
      vinput::cli::ResolveScriptSelectorByShortId(selector, entries, "ASR provider", &error);
  if (id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto it = std::ranges::find_if(
      entries, [&id](const vinput::script::RegistryEntry& entry) { return entry.id == id; });
  if (it == entries.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("ASR provider '%s' not found in registry."), id));
    return 1;
  }

  std::filesystem::path scriptPath;
  if (!vinput::script::DownloadScript(*it, vinput::script::Kind::kAsrProvider, &scriptPath,
                                      &error)) {
    fmt.PrintError(error);
    return 1;
  }
  if (!vinput::script::MaterializeAsrProvider(&config, *it, scriptPath, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  NormalizeCoreConfig(&config);
  if (!SaveAsrConfigAndReload(config, fmt)) {
    return 1;
  }

  fmt.PrintSuccess(vinput::str::FmtStr(_("ASR provider '%s' added."), selector));
  return 0;
}

int RunAsrConfigUpdateProvider(const std::string& selector, bool all, bool force, Formatter& fmt,
                               const CliContext& ctx) {
  (void)ctx;
  if (!all && selector.empty()) {
    fmt.PrintError(_(
        "Specify a provider short ID to update, or use --all to update all installed providers."));
    return 1;
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const auto registryUrls = ResolveAsrProviderRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(_("No ASR provider registry base URLs configured. Edit config.json and set "
                     "registry.base_urls."));
    return 1;
  }

  std::string error;
  const auto entries = vinput::script::FetchRegistry(config, vinput::script::Kind::kAsrProvider,
                                                     registryUrls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto manifest = vinput::registry::InstalledManifest::Load();

  auto updateSingle = [&](const vinput::script::RegistryEntry& entry) -> bool {
    const auto version_info = vinput::script::CheckResourceVersion(
        manifest, vinput::script::Kind::kAsrProvider, entry, true);
    if (!force && version_info.status == vinput::script::UpdateStatus::kUpToDate) {
      fmt.PrintInfo(vinput::str::FmtStr(_("Provider '%s' is already up to date."), entry.short_id));
      return true;
    }

    std::filesystem::path scriptPath;
    if (!vinput::script::DownloadScript(entry, vinput::script::Kind::kAsrProvider, &scriptPath,
                                        &error)) {
      fmt.PrintError(vinput::str::FmtStr(_("Failed to update '%s': %s"), entry.short_id, error));
      return false;
    }
    if (!vinput::script::MaterializeAsrProvider(&config, entry, scriptPath, &error)) {
      fmt.PrintError(vinput::str::FmtStr(_("Failed to update '%s': %s"), entry.short_id, error));
      return false;
    }

    if (!version_info.installed_version.empty() && !version_info.remote_version.empty()) {
      fmt.PrintSuccess(vinput::str::FmtStr(_("ASR provider '%s' updated: %s -> %s"), entry.short_id,
                                           version_info.installed_version,
                                           version_info.remote_version));
    } else {
      fmt.PrintSuccess(vinput::str::FmtStr(_("ASR provider '%s' updated."), entry.short_id));
    }
    return true;
  };

  bool any_updated = false;
  if (all) {
    for (const auto& provider : config.asr.providers) {
      if (std::holds_alternative<LocalAsrProvider>(provider)) {
        continue;
      }
      const std::string id = AsrProviderId(provider);
      auto it = std::ranges::find_if(entries, [&id](const auto& e) { return e.id == id; });
      if (it != entries.end()) {
        if (updateSingle(*it)) {
          any_updated = true;
        }
      }
    }
  } else {
    const std::string id =
        vinput::cli::ResolveScriptSelectorByShortId(selector, entries, "ASR provider", &error);
    if (id.empty()) {
      fmt.PrintError(error);
      return 1;
    }
    auto it = std::ranges::find_if(entries, [&id](const auto& e) { return e.id == id; });
    if (it == entries.end()) {
      fmt.PrintError(vinput::str::FmtStr(_("ASR provider '%s' not found in registry."), selector));
      return 1;
    }
    const auto* existing = ResolveAsrProvider(config, it->id);
    if (existing == nullptr) {
      fmt.PrintError(vinput::str::FmtStr(
          _("ASR provider '%s' is not installed. Use 'add' instead."), selector));
      return 1;
    }
    if (!updateSingle(*it)) {
      return 1;
    }
    any_updated = true;
  }

  if (any_updated) {
    NormalizeCoreConfig(&config);
    if (!SaveAsrConfigAndReload(config, fmt)) {
      return 1;
    }
  }

  return 0;
}

int RunAsrConfigEdit(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledAsrProviderSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  const AsrProvider* provider = ResolveAsrProvider(config, resolved_id);
  if (provider == nullptr) {
    fmt.PrintError(vinput::str::FmtStr(_("ASR provider '%s' not found."), id));
    return 1;
  }
  if (!IsCommandProvider(*provider)) {
    fmt.PrintError(vinput::str::FmtStr(
        _("ASR provider '%s' is not a command provider and cannot be edited."), id));
    return 1;
  }

  const std::filesystem::path scriptPath = ResolveEditableScriptPath(*provider);
  if (scriptPath.empty()) {
    fmt.PrintError(vinput::str::FmtStr(
        _("ASR provider '%s' does not reference an editable script file."), id));
    return 1;
  }

  const int ret = OpenInEditor(scriptPath);
  if (ret != 0) {
    fmt.PrintError(_("Editor exited with error."));
    return ret;
  }

  fmt.PrintSuccess(vinput::str::FmtStr(_("ASR provider '%s' updated."), id));
  return 0;
}

#if VINPUT_ENABLE_LOCAL_ASR
int RunAsrConfigListModels(bool available, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const ModelManager manager(ResolveModelBaseDir(config).string());
  const std::string activeModel = ResolvePreferredLocalModel(config);
  const auto installed_display_map = vinput::cli::FetchModelDisplayMap(config);

  if (available) {
    const auto registryUrls = ResolveModelRegistryUrls(config);
    if (registryUrls.empty()) {
      fmt.PrintError(_(
          "No model registry base URLs configured. Edit config.json and set registry.base_urls."));
      return 1;
    }

    const ModelRepository repository(ResolveModelBaseDir(config).string());
    std::string error;
    const auto remoteModels = ModelRepository::FetchRegistry(config, registryUrls, &error);
    if (!error.empty()) {
      fmt.PrintError(error);
      return 1;
    }

    const auto locale = vinput::registry::DetectPreferredLocale();
    const auto i18nMap = vinput::registry::FetchMergedI18nMap(config, locale);
    const auto display_map = vinput::cli::BuildModelDisplayMap(remoteModels, i18nMap);
    const auto installedModels = manager.ListDetailed(activeModel);

    auto isInstalled = [&installedModels](const std::string& id) {
      return std::ranges::any_of(installedModels,
                                 [&id](const ModelSummary& model) { return model.id == id; });
    };

    if (ctx.json_output) {
      glz::json_t::array_t arr;
      arr.reserve(remoteModels.size());
      for (const auto& model : remoteModels) {
        glz::json_t json_entry = {
            {"id", vinput::cli::HumanizeResourceId(model.id, model.short_id)},
            {"machine_id", model.id},
            {"title", display_map.at(model.id).title},
            {"description", display_map.at(model.id).description},
            {"model_type", model.model_type()},
            {"language", model.language},
            {"supports_hotwords", model.supports_hotwords()},
            {"size_bytes", model.size_bytes},
            {"size", vinput::file::FormatFileSize(model.size_bytes)},
            {"status", isInstalled(model.id) ? "installed" : "available"},
        };
        arr.emplace_back(std::move(json_entry));
      }
      fmt.PrintJson(glz::json_t(std::move(arr)));
      return 0;
    }

    const std::vector<std::string> headers = {_("ID"),   _("TITLE"),    _("TYPE"),  _("LANGUAGE"),
                                              _("SIZE"), _("HOTWORDS"), _("STATUS")};
    std::vector<std::vector<std::string>> rows;
    rows.reserve(remoteModels.size());
    for (const auto& model : remoteModels) {
      rows.push_back({vinput::cli::HumanizeResourceId(model.id, model.short_id),
                      display_map.at(model.id).title, model.model_type(), model.language,
                      vinput::file::FormatFileSize(model.size_bytes),
                      model.supports_hotwords() ? _("yes") : _("no"),
                      isInstalled(model.id) ? _("installed") : _("available")});
    }
    fmt.PrintTable(headers, rows);
    return 0;
  }

  const auto models = manager.ListDetailed(activeModel);

  if (ctx.json_output) {
    glz::json_t::array_t arr;
    arr.reserve(models.size());
    for (const auto& model : models) {
      std::string status = "installed";
      if (model.state == ModelState::Active) {
        status = "active";
      } else if (model.state == ModelState::Broken) {
        status = "broken";
      }
      const auto display_it = installed_display_map.find(model.id);
      glz::json_t json_entry = {
          {"id", vinput::cli::HumanizeResourceId(installed_display_map, model.id)},
          {"machine_id", model.id},
          {"title",
           display_it == installed_display_map.end() ? model.id : display_it->second.title},
          {"model_type", model.model_type},
          {"language", model.language},
          {"supports_hotwords", model.supports_hotwords},
          {"size_bytes", model.size_bytes},
          {"size", vinput::file::FormatFileSize(model.size_bytes)},
          {"status", status},
      };
      arr.emplace_back(std::move(json_entry));
    }
    fmt.PrintJson(glz::json_t(std::move(arr)));
    return 0;
  }

  const std::vector<std::string> headers = {_("ID"),   _("TITLE"),    _("TYPE"),  _("LANGUAGE"),
                                            _("SIZE"), _("HOTWORDS"), _("STATUS")};
  std::vector<std::vector<std::string>> rows;
  for (const auto& model : models) {
    std::string status = _("Installed");
    if (model.state == ModelState::Active) {
      status = std::string("[*] ") + _("Active");
    } else if (model.state == ModelState::Broken) {
      status = std::string("[!] ") + _("Broken");
    }
    const auto display_it = installed_display_map.find(model.id);
    rows.push_back({vinput::cli::HumanizeResourceId(installed_display_map, model.id),
                    display_it == installed_display_map.end() ? model.id : display_it->second.title,
                    model.model_type, model.language,
                    vinput::file::FormatFileSize(model.size_bytes),
                    model.supports_hotwords ? _("yes") : _("no"), status});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunAsrConfigInstallModel(const std::string& selector, Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);
  const auto baseDir = ResolveModelBaseDir(config);
  const auto registryUrls = ResolveModelRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(
        _("No model registry base URLs configured. Edit config.json and set registry.base_urls."));
    return 1;
  }

  const ModelRepository repository(baseDir.string());
  std::string error;
  const auto remoteModels = ModelRepository::FetchRegistry(config, registryUrls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const std::string id = vinput::cli::ResolveModelSelectorByShortId(selector, remoteModels, &error);
  if (id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  uint64_t totalSize = 0;
  for (const auto& model : remoteModels) {
    if (model.id == id) {
      totalSize = model.size_bytes;
      break;
    }
  }

  char labelBuf[256];
  snprintf(labelBuf, sizeof(labelBuf), _("Downloading %s..."), selector.c_str());
  ProgressBar bar(labelBuf, totalSize, ctx.is_tty);

  const bool ok = repository.InstallModel(
      config, registryUrls, id,
      [&bar](const InstallProgress& progress) {
        bar.Update(progress.downloaded_bytes, progress.speed_bps);
      },
      &error);
  bar.Finish();

  if (!ok) {
    fmt.PrintError(error);
    return 1;
  }

  fmt.PrintSuccess(vinput::str::FmtStr(_("Model '%s' added."), selector));
  return 0;
}

int RunAsrConfigRemoveModel(const std::string& selector, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  const ModelManager manager(ResolveModelBaseDir(config).string());
  const std::string activeModel = ResolvePreferredLocalModel(config);
  const auto models = manager.ListDetailed(activeModel);
  const auto display_map = vinput::cli::FetchModelDisplayMap(config);

  std::string error;
  const std::string id =
      vinput::cli::ResolveModelSelectorByShortId(selector, models, display_map, &error);
  if (id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  if (!manager.Remove(id, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  if (activeModel == id) {
    if (!SetPreferredLocalModel(&config, "", &error)) {
      fmt.PrintWarning(error);
    } else if (!SaveAsrConfigAndReload(config, fmt)) {
      return 1;
    }
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("Model '%s' removed."), selector));
  return 0;
}

int RunAsrConfigUseModel(const std::string& selector, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  const ModelManager manager(ResolveModelBaseDir(config).string());
  const std::string activeModel = ResolvePreferredLocalModel(config);
  const auto models = manager.ListDetailed(activeModel);
  const auto display_map = vinput::cli::FetchModelDisplayMap(config);

  std::string error;
  const std::string id =
      vinput::cli::ResolveModelSelectorByShortId(selector, models, display_map, &error);
  if (id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  if (!manager.Validate(id, &error)) {
    fmt.PrintError(vinput::str::FmtStr(_("Model '%s' is not valid: %s"), id, error));
    return 1;
  }
  if (!SetPreferredLocalModel(&config, id, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  if (!SaveAsrConfigAndReload(config, fmt)) {
    return 1;
  }

  fmt.PrintSuccess(vinput::str::FmtStr(_("Preferred local model set to '%s'."), selector));
  return 0;
}

int RunAsrConfigModelInfo(const std::string& selector, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const std::string baseDir = ResolveModelBaseDir(config).string();
  const ModelManager manager(baseDir);
  const std::string activeModel = ResolvePreferredLocalModel(config);
  const auto models = manager.ListDetailed(activeModel);
  const auto display_map = vinput::cli::FetchModelDisplayMap(config);
  std::string error;
  const std::string id =
      vinput::cli::ResolveModelSelectorByShortId(selector, models, display_map, &error);
  if (id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  if (!manager.Validate(id, &error)) {
    fmt.PrintError(vinput::str::FmtStr(_("Model '%s' is not valid: %s"), id, error));
    return 1;
  }

  const ModelManager selectedManager(baseDir, id);
  const ModelInfo info = selectedManager.GetModelInfo(&error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  glz::json_t obj = glz::json_t(glz::json_t::object_t{});
  obj["id"] = vinput::cli::HumanizeResourceId(display_map, id);
  obj["machine_id"] = id;
  obj["backend"] = info.backend;
  obj["runtime"] = info.runtime;
  obj["family"] = info.family;
  obj["language"] = info.language;
  obj["supports_hotwords"] = info.supports_hotwords;
  obj["size_bytes"] = info.size_bytes;
  obj["recognizer"] = info.recognizer_config;
  obj["model"] = info.model_config;
  obj["resolved_files"] = vinput::json::FromStringMap(info.files);

  if (ctx.json_output) {
    fmt.PrintJson(obj);
    return 0;
  }

  const std::vector<std::string> headers = {_("FIELD"), _("VALUE")};
  std::vector<std::vector<std::string>> rows = {
      {"id", vinput::cli::HumanizeResourceId(display_map, id)},
      {"backend", info.backend},
      {"runtime", info.runtime},
      {"family", info.family},
      {"language", info.language},
      {"supports_hotwords", info.supports_hotwords ? "true" : "false"},
      {"size_bytes", std::to_string(info.size_bytes)},
  };
  for (const auto& [key, value] : info.files) {
    rows.push_back({std::string("file.") + key, value});
  }
  rows.push_back({"recognizer", vinput::json::ToText(info.recognizer_config)});
  rows.push_back({"model", vinput::json::ToText(info.model_config)});
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunAsrConfigGetHotword(Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const auto* provider = PreferredLocalProvider(config);
  const std::string hotwordPath = provider != nullptr ? provider->hotwordsFile : "";

  if (ctx.json_output) {
    fmt.PrintJson({{"hotwords_file", hotwordPath}});
    return 0;
  }

  if (hotwordPath.empty()) {
    fmt.PrintInfo(_("No hotwords file configured."));
  } else {
    fmt.PrintInfo(hotwordPath);
  }
  return 0;
}

int RunAsrConfigSetHotword(const std::string& path, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  auto* provider = PreferredLocalProvider(&config);
  if (provider == nullptr) {
    fmt.PrintError(_("No local ASR provider configured."));
    return 1;
  }
  provider->hotwordsFile = path;
  if (!SaveAsrConfigAndReload(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(_("Hotwords file path saved."));
  return 0;
}

int RunAsrConfigClearHotword(Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  auto* provider = PreferredLocalProvider(&config);
  if (provider == nullptr) {
    fmt.PrintError(_("No local ASR provider configured."));
    return 1;
  }
  provider->hotwordsFile.clear();
  if (!SaveAsrConfigAndReload(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(_("Hotwords file path cleared."));
  return 0;
}

int RunAsrConfigEditHotword(Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const CoreConfig config = LoadCoreConfig();
  const auto* provider = PreferredLocalProvider(config);
  const std::string hotwordPath = provider != nullptr ? provider->hotwordsFile : "";
  if (hotwordPath.empty()) {
    fmt.PrintError(_("No hotwords file configured. Use 'hotword set <path>' first."));
    return 1;
  }

  const int result = OpenInEditor(hotwordPath);
  if (result != 0) {
    fmt.PrintError(_("Editor exited with error."));
    return result;
  }
  fmt.PrintSuccess(_("Hotwords file updated."));
  return 0;
}
#endif
