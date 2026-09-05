#include "cli/config/asr_actions.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

#include "common/config/core_config.h"
#include "common/dbus/asr_backend_state_utils.h"
#include "common/i18n.h"
#include "common/registry/registry_i18n.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/path_utils.h"
#include "common/utils/string_utils.h"

#include "config.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include "common/asr/model_manager.h"
#include "common/registry/registry_models.h"
#include "common/utils/download_progress.h"
#endif

#include "cli/runtime/dbus_client.h"
#include "cli/utils/cli_helpers.h"
#include "cli/utils/editor_utils.h"
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
  if (!provider) {
    return nullptr;
  }
  return std::get_if<LocalAsrProvider>(provider);
}

LocalAsrProvider* PreferredLocalProvider(CoreConfig* config) {
  if (!config) {
    return nullptr;
  }
  const AsrProvider* provider = ResolvePreferredLocalAsrProvider(*config);
  if (!provider) {
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
  CoreConfig config = LoadCoreConfig();
  const auto display_map =
      vinput::cli::FetchScriptDisplayMap(config, vinput::script::Kind::kAsrProvider);
#if VINPUT_ENABLE_LOCAL_ASR
  const auto model_display_map = vinput::cli::FetchModelDisplayMap(config);
#endif

  if (ctx.json_output) {
    nlohmann::json providers = nlohmann::json::array();
    for (const auto& provider : config.asr.providers) {
#if !VINPUT_ENABLE_LOCAL_ASR
      if (std::holds_alternative<LocalAsrProvider>(provider)) {
        continue;
      }
#endif
      const std::string machine_id = AsrProviderId(provider);
      nlohmann::json entry = {
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
        entry["args"] = command->args;
        entry["env"] = command->env;
      }
      providers.push_back(std::move(entry));
    }
    fmt.PrintJson(providers);
    return 0;
  }

  std::vector<std::string> headers = {_("ID"),     _("TITLE"), _("TYPE"),
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
  auto it =
      std::find_if(providers.begin(), providers.end(), [&resolved_id](const AsrProvider& provider) {
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

  auto isInstalled = [&config](const std::string& id) {
    return ResolveAsrProvider(config, id) != nullptr;
  };

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& entry : entries) {
      nlohmann::json envs = nlohmann::json::array();
      for (const auto& env : entry.envs) {
        envs.push_back({{"name", env.name}, {"required", env.required}});
      }
      arr.push_back({
          {"id", vinput::cli::HumanizeResourceId(entry.id, entry.short_id)},
          {"machine_id", entry.id},
          {"title", display_map.at(entry.id).title},
          {"description", display_map.at(entry.id).description},
          {"readme_url", entry.readme_url},
          {"stream", entry.stream},
          {"envs", envs},
          {"status", isInstalled(entry.id) ? "installed" : "available"},
      });
    }
    fmt.PrintJson(arr);
    return 0;
  }

  std::vector<std::string> headers = {_("ID"), _("TITLE"), _("MODE"), _("STATUS"), _("README")};
  std::vector<std::vector<std::string>> rows;
  for (const auto& entry : entries) {
    rows.push_back({vinput::cli::HumanizeResourceId(entry.id, entry.short_id),
                    display_map.at(entry.id).title, entry.stream ? _("stream") : _("batch"),
                    isInstalled(entry.id) ? _("installed") : _("available"),
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

  const auto it =
      std::find_if(entries.begin(), entries.end(),
                   [&id](const vinput::script::RegistryEntry& entry) { return entry.id == id; });
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

int RunAsrConfigEdit(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledAsrProviderSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  const AsrProvider* provider = ResolveAsrProvider(config, resolved_id);
  if (!provider) {
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
  CoreConfig config = LoadCoreConfig();
  ModelManager manager(ResolveModelBaseDir(config).string());
  const std::string activeModel = ResolvePreferredLocalModel(config);
  const auto installed_display_map = vinput::cli::FetchModelDisplayMap(config);

  if (available) {
    const auto registryUrls = ResolveModelRegistryUrls(config);
    if (registryUrls.empty()) {
      fmt.PrintError(_(
          "No model registry base URLs configured. Edit config.json and set registry.base_urls."));
      return 1;
    }

    ModelRepository repository(ResolveModelBaseDir(config).string());
    std::string error;
    const auto remoteModels = repository.FetchRegistry(config, registryUrls, &error);
    if (!error.empty()) {
      fmt.PrintError(error);
      return 1;
    }

    const auto locale = vinput::registry::DetectPreferredLocale();
    const auto i18nMap = vinput::registry::FetchMergedI18nMap(config, locale);
    const auto display_map = vinput::cli::BuildModelDisplayMap(remoteModels, i18nMap);
    const auto installedModels = manager.ListDetailed(activeModel);

    auto isInstalled = [&installedModels](const std::string& id) {
      return std::any_of(installedModels.begin(), installedModels.end(),
                         [&id](const ModelSummary& model) { return model.id == id; });
    };

    if (ctx.json_output) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& model : remoteModels) {
        arr.push_back({
            {"id", vinput::cli::HumanizeResourceId(model.id, model.short_id)},
            {"machine_id", model.id},
            {"title", display_map.at(model.id).title},
            {"description", display_map.at(model.id).description},
            {"model_type", model.model_type()},
            {"language", model.language},
            {"supports_hotwords", model.supports_hotwords()},
            {"size_bytes", model.size_bytes},
            {"size", vinput::str::FormatSize(model.size_bytes)},
            {"status", isInstalled(model.id) ? "installed" : "available"},
        });
      }
      fmt.PrintJson(arr);
      return 0;
    }

    std::vector<std::string> headers = {_("ID"),   _("TITLE"),    _("TYPE"),  _("LANGUAGE"),
                                        _("SIZE"), _("HOTWORDS"), _("STATUS")};
    std::vector<std::vector<std::string>> rows;
    for (const auto& model : remoteModels) {
      rows.push_back({vinput::cli::HumanizeResourceId(model.id, model.short_id),
                      display_map.at(model.id).title, model.model_type(), model.language,
                      vinput::str::FormatSize(model.size_bytes),
                      model.supports_hotwords() ? _("yes") : _("no"),
                      isInstalled(model.id) ? _("installed") : _("available")});
    }
    fmt.PrintTable(headers, rows);
    return 0;
  }

  const auto models = manager.ListDetailed(activeModel);

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& model : models) {
      std::string status = "installed";
      if (model.state == ModelState::Active) {
        status = "active";
      } else if (model.state == ModelState::Broken) {
        status = "broken";
      }
      const auto display_it = installed_display_map.find(model.id);
      arr.push_back({
          {"id", vinput::cli::HumanizeResourceId(installed_display_map, model.id)},
          {"machine_id", model.id},
          {"title",
           display_it == installed_display_map.end() ? model.id : display_it->second.title},
          {"model_type", model.model_type},
          {"language", model.language},
          {"supports_hotwords", model.supports_hotwords},
          {"size_bytes", model.size_bytes},
          {"size", vinput::str::FormatSize(model.size_bytes)},
          {"status", status},
      });
    }
    fmt.PrintJson(arr);
    return 0;
  }

  std::vector<std::string> headers = {_("ID"),   _("TITLE"),    _("TYPE"),  _("LANGUAGE"),
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
                    model.model_type, model.language, vinput::str::FormatSize(model.size_bytes),
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

  ModelRepository repository(baseDir.string());
  std::string error;
  const auto remoteModels = repository.FetchRegistry(config, registryUrls, &error);
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
  ModelManager manager(ResolveModelBaseDir(config).string());
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
  ModelManager manager(ResolveModelBaseDir(config).string());
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
  CoreConfig config = LoadCoreConfig();
  const std::string baseDir = ResolveModelBaseDir(config).string();
  ModelManager manager(baseDir);
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

  ModelManager selectedManager(baseDir, id);
  const ModelInfo info = selectedManager.GetModelInfo(&error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  nlohmann::json obj;
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
  obj["resolved_files"] = info.files;

  if (ctx.json_output) {
    fmt.PrintJson(obj);
    return 0;
  }

  std::vector<std::string> headers = {_("FIELD"), _("VALUE")};
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
  rows.push_back({"recognizer", info.recognizer_config.dump()});
  rows.push_back({"model", info.model_config.dump()});
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunAsrConfigGetHotword(Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();
  const auto* provider = PreferredLocalProvider(config);
  const std::string hotwordPath = provider ? provider->hotwordsFile : "";

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
  if (!provider) {
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
  if (!provider) {
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
  CoreConfig config = LoadCoreConfig();
  const auto* provider = PreferredLocalProvider(config);
  const std::string hotwordPath = provider ? provider->hotwordsFile : "";
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
