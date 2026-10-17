#include "cli/config/llm_actions.h"

#include <algorithm>
#include <cstddef>
#include <curl/curl.h>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/types.h>
#include <system_error>
#include <vector>

#include "common/config/core_config.h"
#include "common/i18n.h"
#include "common/llm/adapter_manager.h"
#include "common/llm/defaults.h"
#include "common/llm/provider_model_cache.h"
#include "common/registry/installed_manifest.h"
#include "common/registry/registry_i18n.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/interpolation.h"
#include "common/utils/string_utils.h"

#include "cli/runtime/dbus_client.h"
#include "cli/utils/cli_context.h"
#include "cli/utils/cli_helpers.h"
#include "cli/utils/formatter.h"
#include "cli/utils/resource_utils.h"

namespace {

int SetAdapterAutostart(const std::string& id, bool enable, Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledLlmAdapterSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  auto it = std::find_if(config.llm.adapters.begin(), config.llm.adapters.end(),
                         [&resolved_id](const LlmAdapter& a) { return a.id == resolved_id; });
  if (it == config.llm.adapters.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("Adapter '%s' not found."), id));
    return 1;
  }

  it->autoStart = enable;
  NormalizeCoreConfig(&config);
  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }

  if (ctx.json_output) {
    fmt.PrintJson({{"id", resolved_id}, {"auto_start", it->autoStart}});
    return 0;
  }

  if (enable) {
    fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' enabled to autostart with daemon."), id));
  } else {
    fmt.PrintSuccess(
        vinput::str::FmtStr(_("Adapter '%s' disabled from autostart with daemon."), id));
  }
  return 0;
}

bool ParseExtraBody(const std::string& raw, nlohmann::json* out, Formatter& fmt) {
  if (raw.empty()) {
    *out = nlohmann::json::object();
    return true;
  }
  try {
    auto parsed = nlohmann::json::parse(raw);
    if (!parsed.is_object()) {
      fmt.PrintError(_("--extra-body must be a JSON object (e.g. "
                       "'{\"enable_thinking\": false}')."));
      return false;
    }
    *out = std::move(parsed);
    return true;
  } catch (const std::exception& e) {
    fmt.PrintError(vinput::str::FmtStr(_("Invalid --extra-body JSON: %s"), e.what()));
    return false;
  }
}

} // namespace

int RunLlmConfigList(Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();

  if (ctx.json_output) {
    nlohmann::json providers = nlohmann::json::array();
    for (const auto& provider : config.llm.providers) {
      providers.push_back({
          {"id", provider.id},
          {"base_url", provider.base_url},
          {"api_key", ""},
          {"extra_body",
           provider.extra_body.is_object() ? provider.extra_body : nlohmann::json::object()},
      });
    }
    fmt.PrintJson(providers);
    return 0;
  }

  std::vector<std::string> headers = {_("ID"), _("BASE_URL"), _("API_KEY")};
  std::vector<std::vector<std::string>> rows;
  rows.reserve(config.llm.providers.size());
  for (const auto& provider : config.llm.providers) {
    rows.push_back({provider.id, provider.base_url, vinput::str::MaskSecret(provider.api_key)});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunLlmConfigListAdapters(bool available, Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();
  const auto installed_display_map =
      vinput::cli::FetchScriptDisplayMap(config, vinput::script::Kind::kLlmAdapter);

  if (!available) {
    if (ctx.json_output) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& adapter : config.llm.adapters) {
        const auto it = installed_display_map.find(adapter.id);
        arr.push_back({
            {"id", vinput::cli::HumanizeResourceId(installed_display_map, adapter.id)},
            {"machine_id", adapter.id},
            {"title", it == installed_display_map.end() ? "" : it->second.title},
            {"readme_url", it == installed_display_map.end() ? "" : it->second.readme_url},
            {"command", adapter.command},
            {"args", adapter.args},
            {"env", adapter.env},
            {"auto_start", adapter.autoStart},
        });
      }
      fmt.PrintJson(arr);
      return 0;
    }

    const std::vector<std::string> headers = {_("ID"), _("TITLE"), _("AUTOSTART"), _("README")};
    std::vector<std::vector<std::string>> rows;
    rows.reserve(config.llm.adapters.size());
    for (const auto& adapter : config.llm.adapters) {
      const bool has_entry = installed_display_map.contains(adapter.id);
      rows.push_back(
          {vinput::cli::HumanizeResourceId(installed_display_map, adapter.id),
           has_entry ? installed_display_map.at(adapter.id).title : "",
           adapter.autoStart ? _("enabled") : _("disabled"),
           has_entry ? vinput::cli::FormatTerminalLink(
                           ctx, _("Open README"), installed_display_map.at(adapter.id).readme_url)
                     : ""});
    }
    fmt.PrintTable(headers, rows);
    return 0;
  }

  const auto registryUrls = ResolveLlmAdapterRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(_("No LLM adapter registry base URLs configured. Edit config.json and set "
                     "registry.base_urls."));
    return 1;
  }

  std::string error;
  const auto entries = vinput::script::FetchRegistry(config, vinput::script::Kind::kLlmAdapter,
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
    return ResolveLlmAdapter(config, id) != nullptr;
  };

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& entry : entries) {
      nlohmann::json envs = nlohmann::json::array();
      for (const auto& env : entry.envs) {
        envs.push_back({{"name", env.name}, {"required", env.required}});
      }
      const bool installed = isInstalled(entry.id);
      const auto ver_info = vinput::script::CheckResourceVersion(
          manifest, vinput::script::Kind::kLlmAdapter, entry, installed);
      std::string status_str = "available";
      if (ver_info.status == vinput::script::UpdateStatus::kIncompatible) {
        status_str = "incompatible";
      } else if (installed) {
        status_str = ver_info.status == vinput::script::UpdateStatus::kUpdateAvailable
                         ? "update_available"
                         : "installed";
      }

      arr.push_back({
          {"id", vinput::cli::HumanizeResourceId(entry.id, entry.short_id)},
          {"machine_id", entry.id},
          {"title", display_map.at(entry.id).title},
          {"description", display_map.at(entry.id).description},
          {"readme_url", entry.readme_url},
          {"envs", envs},
          {"status", status_str},
          {"installed_version", ver_info.installed_version},
          {"remote_version", ver_info.remote_version},
          {"vinput_version", ver_info.vinput_version},
      });
    }
    fmt.PrintJson(arr);
    return 0;
  }

  std::vector<std::string> headers = {_("ID"), _("TITLE"), _("STATUS"), _("README")};
  std::vector<std::vector<std::string>> rows;
  for (const auto& entry : entries) {
    const bool installed = isInstalled(entry.id);
    const auto ver_info = vinput::script::CheckResourceVersion(
        manifest, vinput::script::Kind::kLlmAdapter, entry, installed);
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
                    display_map.at(entry.id).title, status_text,
                    vinput::cli::FormatTerminalLink(ctx, _("Open README"), entry.readme_url)});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunLlmConfigAdd(const std::string& id, const std::string& baseUrl, const std::string& apiKey,
                    const std::string& extraBody, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  if (ResolveLlmProvider(config, id) != nullptr) {
    fmt.PrintError(vinput::str::FmtStr(_("LLM provider '%s' already exists."), id));
    return 1;
  }

  nlohmann::json extra_json;
  if (!ParseExtraBody(extraBody, &extra_json, fmt)) {
    return 1;
  }

  LlmProvider provider;
  provider.id = vinput::str::TrimAsciiWhitespace(id);
  provider.base_url = vinput::str::TrimAsciiWhitespace(baseUrl);
  provider.api_key = vinput::str::TrimAsciiWhitespace(apiKey);
  provider.extra_body = std::move(extra_json);
  config.llm.providers.push_back(std::move(provider));

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("LLM provider '%s' added."), id));
  return 0;
}

int RunLlmConfigInstallAdapter(const std::string& selector,
                               const std::vector<std::string>& envOverrides, Formatter& fmt,
                               const CliContext& ctx) {
  (void)ctx;
  std::map<std::string, std::string> overrides;
  for (const auto& item : envOverrides) {
    const auto pair = vinput::str::SplitPair(item, '=');
    if (!pair || pair->first.empty()) {
      // A malformed value may be a token pasted without its key, so never echo it back.
      fmt.PrintError(_("Invalid --env: expected KEY=VALUE."));
      return 1;
    }
    overrides[std::string(pair->first)] = std::string(pair->second);
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const auto registryUrls = ResolveLlmAdapterRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(_("No LLM adapter registry base URLs configured. Edit config.json and set "
                     "registry.base_urls."));
    return 1;
  }

  std::string error;
  const auto entries = vinput::script::FetchRegistry(config, vinput::script::Kind::kLlmAdapter,
                                                     registryUrls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const std::string id =
      vinput::cli::ResolveScriptSelectorByShortId(selector, entries, "LLM adapter", &error);
  if (id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto it =
      std::find_if(entries.begin(), entries.end(),
                   [&id](const vinput::script::RegistryEntry& entry) { return entry.id == id; });
  if (it == entries.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("Adapter '%s' not found in registry."), id));
    return 1;
  }

  std::filesystem::path scriptPath;
  if (!vinput::script::DownloadScript(*it, vinput::script::Kind::kLlmAdapter, &scriptPath,
                                      &error)) {
    fmt.PrintError(error);
    return 1;
  }
  if (!vinput::script::MaterializeLlmAdapter(&config, *it, scriptPath, &error, overrides)) {
    fmt.PrintError(error);
    return 1;
  }
  NormalizeCoreConfig(&config);
  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }

  // The registry can only declare required envs, never default them, so report
  // the ones still blank instead of saving an adapter that cannot start. A
  // whitespace-only value counts as blank: the adapter scripts strip whitespace
  // before testing the value and then reject it at startup.
  const LlmAdapter* installed = ResolveLlmAdapter(config, it->id);
  if (installed != nullptr) {
    for (const auto& spec : it->envs) {
      if (!spec.required) {
        continue;
      }
      const auto value = installed->env.find(spec.name);
      if (value == installed->env.end() ||
          vinput::str::TrimAsciiWhitespace(value->second).empty()) {
        fmt.PrintWarning(vinput::str::FmtStr(
            _("Required env '%s' is empty; '%s' cannot start until it is set (re-run with -e "
              "%s=<value>)."),
            spec.name, selector, spec.name));
      }
    }
  }

  fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' added."), selector));
  return 0;
}

int RunLlmConfigRemoveAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledLlmAdapterSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  if (vinput::adapter::IsRunning(resolved_id)) {
    vinput::cli::DbusClient dbus;
    std::string stop_err;
    if (!dbus.StopAdapter(resolved_id, &stop_err)) {
      fmt.PrintWarning(
          vinput::str::FmtStr(_("Failed to stop running adapter '%s': %s"), id, stop_err));
    }
  }

  auto it = std::remove_if(config.llm.adapters.begin(), config.llm.adapters.end(),
                           [&resolved_id](const LlmAdapter& a) { return a.id == resolved_id; });
  if (it == config.llm.adapters.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("Adapter '%s' not found."), id));
    return 1;
  }
  config.llm.adapters.erase(it, config.llm.adapters.end());

  const auto managed_path =
      vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kLlmAdapter, resolved_id);
  if (!managed_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(managed_path, ec);
  }
  vinput::script::UnregisterInstalledScript(resolved_id);

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' removed."), id));
  return 0;
}

int RunLlmConfigUpdateAdapter(const std::string& selector, bool all, bool force, Formatter& fmt,
                              const CliContext& ctx) {
  (void)ctx;
  if (!all && selector.empty()) {
    fmt.PrintError(
        _("Specify an adapter short ID to update, or use --all to update all installed adapters."));
    return 1;
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const auto registryUrls = ResolveLlmAdapterRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(_("No LLM adapter registry base URLs configured. Edit config.json and set "
                     "registry.base_urls."));
    return 1;
  }

  std::string error;
  const auto entries = vinput::script::FetchRegistry(config, vinput::script::Kind::kLlmAdapter,
                                                     registryUrls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto manifest = vinput::registry::InstalledManifest::Load();
  vinput::cli::DbusClient dbus;

  auto updateSingle = [&](const vinput::script::RegistryEntry& entry) -> bool {
    const auto version_info = vinput::script::CheckResourceVersion(
        manifest, vinput::script::Kind::kLlmAdapter, entry, true);
    if (!force && version_info.status == vinput::script::UpdateStatus::kUpToDate) {
      fmt.PrintInfo(vinput::str::FmtStr(_("Adapter '%s' is already up to date."), entry.short_id));
      return true;
    }

    const bool was_running = vinput::adapter::IsRunning(entry.id);
    if (was_running) {
      std::string stop_err;
      if (!dbus.StopAdapter(entry.id, &stop_err)) {
        fmt.PrintWarning(vinput::str::FmtStr(_("Could not stop adapter '%s' before update: %s"),
                                             entry.short_id, stop_err));
      }
    }

    std::filesystem::path scriptPath;
    if (!vinput::script::DownloadScript(entry, vinput::script::Kind::kLlmAdapter, &scriptPath,
                                        &error)) {
      fmt.PrintError(vinput::str::FmtStr(_("Failed to update '%s': %s"), entry.short_id, error));
      return false;
    }
    if (!vinput::script::MaterializeLlmAdapter(&config, entry, scriptPath, &error)) {
      fmt.PrintError(vinput::str::FmtStr(_("Failed to update '%s': %s"), entry.short_id, error));
      return false;
    }

    if (was_running) {
      std::string start_err;
      if (dbus.StartAdapter(entry.id, &start_err)) {
        fmt.PrintInfo(vinput::str::FmtStr(_("Adapter '%s' restarted."), entry.short_id));
      } else {
        fmt.PrintWarning(vinput::str::FmtStr(_("Failed to restart adapter '%s': %s"),
                                             entry.short_id, start_err));
      }
    }

    if (!version_info.installed_version.empty() && !version_info.remote_version.empty()) {
      fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' updated: %s -> %s"), entry.short_id,
                                           version_info.installed_version,
                                           version_info.remote_version));
    } else {
      fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' updated."), entry.short_id));
    }
    return true;
  };

  bool any_updated = false;
  if (all) {
    for (const auto& adapter : config.llm.adapters) {
      auto it = std::find_if(entries.begin(), entries.end(),
                             [&](const auto& e) { return e.id == adapter.id; });
      if (it != entries.end()) {
        if (updateSingle(*it)) {
          any_updated = true;
        }
      }
    }
  } else {
    const std::string id =
        vinput::cli::ResolveScriptSelectorByShortId(selector, entries, "LLM adapter", &error);
    if (id.empty()) {
      fmt.PrintError(error);
      return 1;
    }
    auto it =
        std::find_if(entries.begin(), entries.end(), [&id](const auto& e) { return e.id == id; });
    if (it == entries.end()) {
      fmt.PrintError(vinput::str::FmtStr(_("Adapter '%s' not found in registry."), selector));
      return 1;
    }
    const auto* existing = ResolveLlmAdapter(config, it->id);
    if (existing == nullptr) {
      fmt.PrintError(
          vinput::str::FmtStr(_("Adapter '%s' is not installed. Use 'add' instead."), selector));
      return 1;
    }
    if (!updateSingle(*it)) {
      return 1;
    }
    any_updated = true;
  }

  if (any_updated) {
    NormalizeCoreConfig(&config);
    if (!SaveConfigOrFail(config, fmt)) {
      return 1;
    }
  }

  return 0;
}

int RunLlmConfigStartAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledLlmAdapterSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  vinput::cli::DbusClient dbus;
  if (!dbus.StartAdapter(resolved_id, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' started."), id));
  return 0;
}

int RunLlmConfigStopAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledLlmAdapterSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  vinput::cli::DbusClient dbus;
  if (!dbus.StopAdapter(resolved_id, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' stopped."), id));
  return 0;
}

int RunLlmConfigRestartAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledLlmAdapterSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  vinput::cli::DbusClient dbus;
  if (vinput::adapter::IsRunning(resolved_id)) {
    if (!dbus.StopAdapter(resolved_id, &error)) {
      fmt.PrintError(error);
      return 1;
    }
  }
  if (!dbus.StartAdapter(resolved_id, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' restarted."), id));
  return 0;
}

int RunLlmConfigEnableAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  return SetAdapterAutostart(id, true, fmt, ctx);
}

int RunLlmConfigDisableAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  return SetAdapterAutostart(id, false, fmt, ctx);
}

int RunLlmConfigPsAdapters(Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const auto installed_display_map =
      vinput::cli::FetchScriptDisplayMap(config, vinput::script::Kind::kLlmAdapter);

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& adapter : config.llm.adapters) {
      const auto it = installed_display_map.find(adapter.id);
      const pid_t pid = vinput::adapter::GetPid(adapter.id);
      const bool running = (pid > 0);
      arr.push_back({
          {"id", vinput::cli::HumanizeResourceId(installed_display_map, adapter.id)},
          {"machine_id", adapter.id},
          {"title", it == installed_display_map.end() ? "" : it->second.title},
          {"status", running ? "running" : "stopped"},
          {"running", running},
          {"pid", running ? nlohmann::json(pid) : nullptr},
          {"auto_start", adapter.autoStart},
          {"command", adapter.command},
      });
    }
    fmt.PrintJson(arr);
    return 0;
  }

  const std::vector<std::string> headers = {_("ID"), _("STATUS"), _("PID"), _("AUTOSTART"),
                                            _("COMMAND")};
  std::vector<std::vector<std::string>> rows;
  rows.reserve(config.llm.adapters.size());
  for (const auto& adapter : config.llm.adapters) {
    const pid_t pid = vinput::adapter::GetPid(adapter.id);
    const bool running = (pid > 0);
    rows.push_back({
        vinput::cli::HumanizeResourceId(installed_display_map, adapter.id),
        running ? _("running") : _("stopped"),
        running ? std::to_string(pid) : "-",
        adapter.autoStart ? _("enabled") : _("disabled"),
        adapter.command,
    });
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunLlmConfigStatusAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledLlmAdapterSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto* adapter = ResolveLlmAdapter(config, resolved_id);
  if (adapter == nullptr) {
    fmt.PrintError(vinput::str::FmtStr(_("Adapter '%s' not found."), id));
    return 1;
  }

  const auto installed_display_map =
      vinput::cli::FetchScriptDisplayMap(config, vinput::script::Kind::kLlmAdapter);
  const auto it = installed_display_map.find(adapter->id);
  const std::string title = (it == installed_display_map.end()) ? "" : it->second.title;
  const pid_t pid = vinput::adapter::GetPid(adapter->id);
  const bool running = (pid > 0);

  if (ctx.json_output) {
    fmt.PrintJson({
        {"id", vinput::cli::HumanizeResourceId(installed_display_map, adapter->id)},
        {"machine_id", adapter->id},
        {"title", title},
        {"status", running ? "running" : "stopped"},
        {"running", running},
        {"pid", running ? nlohmann::json(pid) : nullptr},
        {"auto_start", adapter->autoStart},
        {"command", adapter->command},
        {"args", adapter->args},
        {"env", adapter->env},
    });
    return 0;
  }

  const std::vector<std::string> headers = {_("PROPERTY"), _("VALUE")};
  std::vector<std::vector<std::string>> rows = {
      {_("ID"), vinput::cli::HumanizeResourceId(installed_display_map, adapter->id)},
      {_("Title"), title},
      {_("Status"), running ? _("running") : _("stopped")},
      {_("PID"), running ? std::to_string(pid) : "-"},
      {_("Autostart"), adapter->autoStart ? _("enabled") : _("disabled")},
      {_("Command"), adapter->command},
  };
  if (!adapter->args.empty()) {
    rows.push_back({_("Args"), vinput::str::Join(adapter->args, " ")});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunLlmConfigRemove(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  auto& providers = config.llm.providers;
  const auto it = std::ranges::find_if(
      providers, [&id](const LlmProvider& provider) { return provider.id == id; });
  if (it == providers.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("LLM provider '%s' not found."), id));
    return 1;
  }

  providers.erase(it);
  vinput::llm::RemoveProviderModels(id);
  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("LLM provider '%s' removed."), id));
  return 0;
}

int RunLlmConfigEdit(const std::string& id, const std::string& baseUrl, const std::string& apiKey,
                     const std::string& extraBody, bool hasBaseUrl, bool hasApiKey,
                     bool hasExtraBody, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  auto& providers = config.llm.providers;
  const auto it = std::ranges::find_if(
      providers, [&id](const LlmProvider& provider) { return provider.id == id; });
  if (it == providers.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("LLM provider '%s' not found."), id));
    return 1;
  }

  if (hasBaseUrl) {
    it->base_url = vinput::str::TrimAsciiWhitespace(baseUrl);
  }
  if (hasApiKey) {
    it->api_key = vinput::str::TrimAsciiWhitespace(apiKey);
  }
  if (hasExtraBody) {
    nlohmann::json extra_json;
    if (!ParseExtraBody(extraBody, &extra_json, fmt)) {
      return 1;
    }
    it->extra_body = std::move(extra_json);
  }

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("LLM provider '%s' updated."), id));
  return 0;
}

namespace {

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  const size_t total = size * nmemb;
  if (!userdata || !ptr || total == 0)
    return 0;
  auto* buf = static_cast<std::string*>(userdata);
  if (buf->size() + total > 1 * 1024 * 1024)
    return 0;
  buf->append(ptr, total);
  return total;
}

} // namespace

int RunLlmConfigTest(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();
  const auto* provider = ResolveLlmProvider(config, id);
  if (!provider) {
    fmt.PrintError(vinput::str::FmtStr(_("LLM provider '%s' not found."), id));
    return 1;
  }

  if (provider->base_url.empty()) {
    fmt.PrintError(_("Provider base_url is empty."));
    return 1;
  }

  std::string url = provider->base_url;
  while (!url.empty() && url.back() == '/')
    url.pop_back();
  url += vinput::llm::kOpenAiModelsPath;

  CURL* curl = curl_easy_init();
  if (!curl) {
    fmt.PrintError(_("Failed to initialize libcurl."));
    return 1;
  }

  struct curl_slist* headers = nullptr;
  const std::string resolved_api_key = vinput::interpolate::InterpolateEnv(provider->api_key);
  if (vinput::interpolate::HasInterpolation(provider->api_key) && resolved_api_key.empty()) {
    fmt.PrintWarning(vinput::str::FmtStr(
        _("Environment variable '%s' referenced in API key for provider '%s' is unset or empty."),
        provider->api_key, provider->id));
  }
  if (!resolved_api_key.empty()) {
    std::string auth = std::string(vinput::llm::kAuthorizationHeader) + ": " +
                       vinput::llm::kBearerPrefix + resolved_api_key;
    headers = curl_slist_append(headers, auth.c_str());
  }

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, vinput::llm::kHttpUserAgent);

  CURLcode code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

  if (headers)
    curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    fmt.PrintError(vinput::str::FmtStr(_("Connection failed: %s"), curl_easy_strerror(code)));
    return 1;
  }

  if (status_code < 200 || status_code >= 300) {
    fmt.PrintError(vinput::str::FmtStr(_("HTTP %ld: %s"), status_code, response_body));
    return 1;
  }

  nlohmann::json response;
  try {
    response = nlohmann::json::parse(response_body);
  } catch (const std::exception& e) {
    fmt.PrintError(vinput::str::FmtStr(_("Invalid JSON response: %s"), e.what()));
    return 1;
  }

  std::vector<std::string> models;
  if (response.contains("data") && response["data"].is_array()) {
    for (const auto& item : response["data"]) {
      if (item.contains("id") && item["id"].is_string()) {
        models.push_back(item["id"].get<std::string>());
      }
    }
  }

  vinput::llm::SaveProviderModels(id, models);

  if (ctx.json_output) {
    nlohmann::json result = {
        {"status", "ok"},
        {"provider", id},
        {"models", models},
    };
    fmt.PrintJson(result);
    return 0;
  }

  fmt.PrintSuccess(
      vinput::str::FmtStr(_("Connected to '%s'. Found %d model(s)."), id, (int)models.size()));
  if (!models.empty()) {
    std::sort(models.begin(), models.end());
    std::vector<std::string> headers_row = {_("MODEL")};
    std::vector<std::vector<std::string>> rows;
    for (const auto& m : models) {
      rows.push_back({m});
    }
    fmt.PrintTable(headers_row, rows);
  }
  return 0;
}
