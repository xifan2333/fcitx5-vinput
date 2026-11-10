#include "cli/config/llm_actions.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fmt/printf.h>
#include <glaze/core/common.hpp>
#include <glaze/core/context.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/i18n.h"
#include "common/json/json_access.h"
#include "common/llm/defaults.h"
#include "common/llm/provider_model_cache.h"
#include "common/net/http_client.h"
#include "common/net/http_types.h"
#include "common/registry/installed_manifest.h"
#include "common/registry/registry_i18n.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/interpolation.h"
#include "common/utils/string_utils.h"

#include "cli/utils/cli_context.h"
#include "cli/utils/cli_helpers.h"
#include "cli/utils/editor_utils.h"
#include "cli/utils/formatter.h"
#include "cli/utils/resource_utils.h"

namespace {

std::string MaskSecret(std::string_view secret, size_t head_keep = 4, size_t tail_keep = 4,
                       char mask_char = '*') {
  if (secret.empty()) {
    return {};
  }
  std::string result;
  result.reserve(secret.size());
  if (tail_keep >= secret.size() || head_keep >= secret.size() - tail_keep) {
    result.assign(secret.size(), mask_char);
    return result;
  }
  result.append(secret.substr(0, head_keep));
  result.append(secret.size() - head_keep - tail_keep, mask_char);
  result.append(secret.substr(secret.size() - tail_keep));
  return result;
}

bool ParseExtraBody(const std::string& raw, glz::json_t* out, Formatter& fmt) {
  *out = glz::json_t(glz::json_t::object_t{});
  if (raw.empty()) {
    return true;
  }
  if (!vinput::json::IsSingleJsonValue(raw)) {
    // Reuse the parse error message so no new translatable string is needed.
    const glz::error_ctx trailing{.ec = glz::error_code::syntax_error, .location = raw.size()};
    fmt.PrintError(
        ::fmt::sprintf(_("Invalid --extra-body JSON: %s"), glz::format_error(trailing).c_str()));
    return false;
  }
  glz::json_t parsed;
  if (const auto ec = glz::read_json(parsed, raw); bool(ec)) {
    fmt.PrintError(
        ::fmt::sprintf(_("Invalid --extra-body JSON: %s"), glz::format_error(ec, raw).c_str()));
    return false;
  }
  if (!parsed.is_object()) {
    fmt.PrintError(_("--extra-body must be a JSON object (e.g. "
                     "'{\"enable_thinking\": false}')."));
    return false;
  }
  *out = std::move(parsed);
  return true;
}

} // namespace

int RunLlmConfigList(Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();

  if (ctx.json_output) {
    glz::json_t::array_t providers;
    providers.reserve(config.llm.providers.size());
    for (const auto& provider : config.llm.providers) {
      glz::json_t json_entry = {
          {"id", provider.id},
          {"base_url", provider.base_url},
          {"api_key", ""},
          {"extra_body", provider.extra_body.is_object() ? provider.extra_body
                                                         : glz::json_t(glz::json_t::object_t{})},
      };
      providers.emplace_back(std::move(json_entry));
    }
    fmt.PrintJson(glz::json_t(std::move(providers)));
    return 0;
  }

  const std::vector<std::string> headers = {_("ID"), _("BASE_URL"), _("API_KEY")};
  std::vector<std::vector<std::string>> rows;
  rows.reserve(config.llm.providers.size());
  for (const auto& provider : config.llm.providers) {
    rows.push_back({provider.id, provider.base_url, MaskSecret(provider.api_key)});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

namespace {

void RenderInstalledAdaptersJson(const std::vector<LlmAdapter>& adapters,
                                 const vinput::cli::ResourceDisplayMap& installed_display_map,
                                 Formatter& fmt) {
  glz::json_t::array_t arr;
  arr.reserve(adapters.size());
  for (const auto& adapter : adapters) {
    const auto it = installed_display_map.find(adapter.id);
    glz::json_t json_entry = {
        {"id", vinput::cli::HumanizeResourceId(installed_display_map, adapter.id)},
        {"machine_id", adapter.id},
        {"title", it == installed_display_map.end() ? "" : it->second.title},
        {"readme_url", it == installed_display_map.end() ? "" : it->second.readme_url},
        {"command", adapter.command},
        {"args", vinput::json::FromStrings(adapter.args)},
        {"env", vinput::json::FromStringMap(adapter.env)},
    };
    arr.emplace_back(std::move(json_entry));
  }
  fmt.PrintJson(glz::json_t(std::move(arr)));
}

void RenderInstalledAdaptersTable(const std::vector<LlmAdapter>& adapters,
                                  const vinput::cli::ResourceDisplayMap& installed_display_map,
                                  Formatter& fmt, const CliContext& ctx) {
  const std::vector<std::string> headers = {_("ID"), _("TITLE"), _("COMMAND"), _("README")};
  std::vector<std::vector<std::string>> rows;
  rows.reserve(adapters.size());
  for (const auto& adapter : adapters) {
    const bool has_entry = installed_display_map.contains(adapter.id);
    rows.push_back(
        {vinput::cli::HumanizeResourceId(installed_display_map, adapter.id),
         has_entry ? installed_display_map.at(adapter.id).title : "", adapter.command,
         has_entry ? vinput::cli::FormatTerminalLink(
                         ctx, _("Open README"), installed_display_map.at(adapter.id).readme_url)
                   : ""});
  }
  fmt.PrintTable(headers, rows);
}

} // namespace

int RunLlmConfigListAdapters(bool available, Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();

  if (available) {
    return vinput::cli::RunRegistryScriptList(
        config, vinput::script::Kind::kLlmAdapter,
        [&config](const std::string& id) {
          return CoreConfigView(config).LlmAdapter(id) != nullptr;
        },
        fmt, ctx);
  }

  const auto installed_display_map =
      vinput::cli::FetchScriptDisplayMap(config, vinput::script::Kind::kLlmAdapter);
  if (ctx.json_output) {
    RenderInstalledAdaptersJson(config.llm.adapters, installed_display_map, fmt);
    return 0;
  }

  RenderInstalledAdaptersTable(config.llm.adapters, installed_display_map, fmt, ctx);
  return 0;
}

int RunLlmConfigAdd(const std::string& id, const std::string& baseUrl, const std::string& apiKey,
                    const std::string& extraBody, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  if (CoreConfigView(config).LlmProvider(id) != nullptr) {
    fmt.PrintError(::fmt::sprintf(_("LLM provider '%s' already exists."), id));
    return 1;
  }

  glz::json_t extra_json;
  if (!ParseExtraBody(extraBody, &extra_json, fmt)) {
    return 1;
  }

  LlmProvider provider;
  provider.id = std::string(vinput::str::Trim(id));
  provider.base_url = std::string(vinput::str::Trim(baseUrl));
  provider.api_key = std::string(vinput::str::Trim(apiKey));
  provider.extra_body = std::move(extra_json);
  config.llm.providers.push_back(std::move(provider));

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(::fmt::sprintf(_("LLM provider '%s' added."), id));
  return 0;
}

namespace {

void ValidateRequiredEnvs(const LlmAdapter* installed, const vinput::script::RegistryEntry& entry,
                          const std::string& selector, Formatter& fmt) {
  if (installed == nullptr) {
    return;
  }
  for (const auto& spec : entry.envs) {
    if (!spec.required) {
      continue;
    }
    const auto value = installed->env.find(spec.name);
    if (value == installed->env.end() || std::string(vinput::str::Trim(value->second)).empty()) {
      fmt.PrintWarning(::fmt::sprintf(
          _("Required env '%s' is empty; '%s' cannot start until it is set (re-run with -e "
            "%s=<value>)."),
          spec.name, selector, spec.name));
    }
  }
}

} // namespace

int RunLlmConfigInstallAdapter(const std::string& selector,
                               const std::vector<std::string>& envOverrides, Formatter& fmt,
                               const CliContext& ctx) {
  (void)ctx;
  std::map<std::string, std::string> overrides;
  if (!vinput::cli::ParseEnvOverrides(envOverrides, &overrides, fmt)) {
    return 1;
  }

  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const auto entries_opt =
      vinput::cli::FetchScriptRegistryEntries(config, vinput::script::Kind::kLlmAdapter, fmt);
  if (!entries_opt) {
    return 1;
  }
  const auto& entries = *entries_opt;

  const auto* entry = vinput::cli::FindRegistryScriptEntry(
      entries, vinput::script::Kind::kLlmAdapter, selector, fmt);
  if (entry == nullptr) {
    return 1;
  }

  const auto scriptPath =
      vinput::cli::DownloadRegistryScript(*entry, vinput::script::Kind::kLlmAdapter, fmt);
  if (!scriptPath) {
    return 1;
  }

  std::string error;
  if (!vinput::script::MaterializeLlmAdapter(&config, *entry, *scriptPath, &error, overrides)) {
    fmt.PrintError(error);
    return 1;
  }
  NormalizeCoreConfig(&config);
  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }

  ValidateRequiredEnvs(CoreConfigView(config).LlmAdapter(entry->id), *entry, selector, fmt);
  fmt.PrintSuccess(::fmt::sprintf(_("Adapter '%s' added."), selector));
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

  const auto removed = std::ranges::remove_if(
      config.llm.adapters, [&resolved_id](const LlmAdapter& a) { return a.id == resolved_id; });
  if (removed.empty()) {
    fmt.PrintError(::fmt::sprintf(_("Adapter '%s' not found."), id));
    return 1;
  }
  config.llm.adapters.erase(removed.begin(), removed.end());

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
  fmt.PrintSuccess(::fmt::sprintf(_("Adapter '%s' removed."), id));
  return 0;
}

namespace {

bool UpdateSingleLlmAdapter(CoreConfig* config, const vinput::registry::InstalledManifest& manifest,
                            const vinput::script::RegistryEntry& entry, bool force,
                            Formatter& fmt) {
  return vinput::cli::UpdateRegistryScript(
      manifest, entry, vinput::script::Kind::kLlmAdapter, force,
      [config, &entry](const std::filesystem::path& path, std::string* error) {
        return vinput::script::MaterializeLlmAdapter(config, entry, path, error);
      },
      fmt);
}

bool UpdateAllLlmAdapters(CoreConfig* config, const vinput::registry::InstalledManifest& manifest,
                          const std::vector<vinput::script::RegistryEntry>& entries, bool force,
                          Formatter& fmt) {
  bool any_updated = false;
  for (const auto& adapter : config->llm.adapters) {
    auto it = std::ranges::find_if(entries, [&](const auto& e) { return e.id == adapter.id; });
    if (it == entries.end()) {
      continue;
    }
    if (UpdateSingleLlmAdapter(config, manifest, *it, force, fmt)) {
      any_updated = true;
    }
  }
  return any_updated;
}

} // namespace

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

  const auto entries_opt =
      vinput::cli::FetchScriptRegistryEntries(config, vinput::script::Kind::kLlmAdapter, fmt);
  if (!entries_opt) {
    return 1;
  }
  const auto& entries = *entries_opt;
  const auto manifest = vinput::registry::InstalledManifest::Load();

  if (all) {
    const bool any_updated = UpdateAllLlmAdapters(&config, manifest, entries, force, fmt);
    if (!any_updated) {
      return 0;
    }
    NormalizeCoreConfig(&config);
    return SaveConfigOrFail(config, fmt) ? 0 : 1;
  }

  const auto* entry = vinput::cli::FindRegistryScriptEntry(
      entries, vinput::script::Kind::kLlmAdapter, selector, fmt);
  if (entry == nullptr) {
    return 1;
  }

  if (CoreConfigView(config).LlmAdapter(entry->id) == nullptr) {
    fmt.PrintError(
        ::fmt::sprintf(_("Adapter '%s' is not installed. Use 'add' instead."), selector));
    return 1;
  }

  if (!UpdateSingleLlmAdapter(&config, manifest, *entry, force, fmt)) {
    return 1;
  }

  NormalizeCoreConfig(&config);
  return SaveConfigOrFail(config, fmt) ? 0 : 1;
}

int RunLlmConfigEditAdapter(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  const CoreConfig config = LoadCoreConfig();
  std::string error;
  const std::string resolved_id =
      vinput::cli::ResolveInstalledLlmAdapterSelector(config, id, &error);
  if (resolved_id.empty()) {
    fmt.PrintError(error);
    return 1;
  }
  const auto* adapter = CoreConfigView(config).LlmAdapter(resolved_id);
  if (adapter == nullptr) {
    fmt.PrintError(::fmt::sprintf(_("Adapter '%s' not found."), id));
    return 1;
  }

  auto script_path =
      vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kLlmAdapter, resolved_id);
  std::error_code ec;
  if (!std::filesystem::exists(script_path, ec) || ec) {
    if (!adapter->args.empty()) {
      const std::filesystem::path candidate(adapter->args.front());
      if (std::filesystem::exists(candidate, ec) && !ec) {
        script_path = candidate;
      }
    }
  }

  if (script_path.empty() || !std::filesystem::exists(script_path, ec) || ec) {
    fmt.PrintError(
        ::fmt::sprintf(_("Adapter '%s' does not reference an editable script file."), id));
    return 1;
  }

  const int ret = OpenInEditor(script_path);
  if (ret != 0) {
    fmt.PrintError(_("Editor exited with error."));
    return ret;
  }

  fmt.PrintSuccess(::fmt::sprintf(_("Adapter '%s' updated."), id));
  return 0;
}

namespace {

auto FindLlmProviderOrReport(std::vector<LlmProvider>& providers, const std::string& id,
                             Formatter& fmt) {
  auto it = std::ranges::find_if(providers,
                                 [&id](const LlmProvider& provider) { return provider.id == id; });
  if (it == providers.end()) {
    fmt.PrintError(::fmt::sprintf(_("LLM provider '%s' not found."), id));
  }
  return it;
}

} // namespace

int RunLlmConfigRemove(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  auto it = FindLlmProviderOrReport(config.llm.providers, id, fmt);
  if (it == config.llm.providers.end()) {
    return 1;
  }

  config.llm.providers.erase(it);
  vinput::llm::RemoveProviderModels(id);
  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(::fmt::sprintf(_("LLM provider '%s' removed."), id));
  return 0;
}

int RunLlmConfigEdit(const std::string& id, const std::string& baseUrl, const std::string& apiKey,
                     const std::string& extraBody, bool hasBaseUrl, bool hasApiKey,
                     bool hasExtraBody, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  auto it = FindLlmProviderOrReport(config.llm.providers, id, fmt);
  if (it == config.llm.providers.end()) {
    return 1;
  }

  if (hasBaseUrl) {
    it->base_url = std::string(vinput::str::Trim(baseUrl));
  }
  if (hasApiKey) {
    it->api_key = std::string(vinput::str::Trim(apiKey));
  }
  if (hasExtraBody) {
    glz::json_t extra_json;
    if (!ParseExtraBody(extraBody, &extra_json, fmt)) {
      return 1;
    }
    it->extra_body = std::move(extra_json);
  }

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(::fmt::sprintf(_("LLM provider '%s' updated."), id));
  return 0;
}

namespace {

std::vector<std::string> ExtractModelIdsFromJson(const glz::json_t& response) {
  std::vector<std::string> models;
  const auto* data = vinput::json::Find(response, "data");
  if (data == nullptr || !data->is_array()) {
    return models;
  }
  for (const auto& item : data->get_array()) {
    const auto* model_id = vinput::json::Find(item, "id");
    if (model_id != nullptr && model_id->is_string()) {
      models.push_back(model_id->get<std::string>());
    }
  }
  return models;
}

void RenderTestModelsTable(std::vector<std::string> models, Formatter& fmt) {
  std::ranges::sort(models);
  const std::vector<std::string> headers_row = {_("MODEL")};
  std::vector<std::vector<std::string>> rows;
  rows.reserve(models.size());
  for (const auto& m : models) {
    rows.push_back({m});
  }
  fmt.PrintTable(headers_row, rows);
}

} // namespace

int RunLlmConfigTest(const std::string& id, Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const auto* provider = CoreConfigView(config).LlmProvider(id);
  if (provider == nullptr) {
    fmt.PrintError(::fmt::sprintf(_("LLM provider '%s' not found."), id));
    return 1;
  }

  if (provider->base_url.empty()) {
    fmt.PrintError(_("Provider base_url is empty."));
    return 1;
  }

  std::string url = provider->base_url;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  url += vinput::llm::kOpenAiModelsPath;

  const std::string resolved_api_key = vinput::interpolate::InterpolateEnv(provider->api_key);
  if (vinput::interpolate::HasInterpolation(provider->api_key) && resolved_api_key.empty()) {
    fmt.PrintWarning(::fmt::sprintf(
        _("Environment variable '%s' referenced in API key for provider '%s' is unset or empty."),
        provider->api_key, provider->id));
  }

  vinput::net::HttpRequest req;
  req.url = url;
  req.timeout_ms = 10000;
  req.max_response_bytes = 1024ULL * 1024ULL;
  req.follow_redirects = true;
  req.headers.push_back({"User-Agent", std::string(vinput::llm::kHttpUserAgent)});
  if (!resolved_api_key.empty()) {
    req.headers.push_back({std::string(vinput::llm::kAuthorizationHeader),
                           std::string(vinput::llm::kBearerPrefix) + resolved_api_key});
  }

  const auto res = vinput::net::HttpClient::Instance().Request(req);
  if (!res.Ok()) {
    if (res.status_code == 0) {
      fmt.PrintError(::fmt::sprintf(_("Connection failed: %s"), res.error_message));
    } else {
      fmt.PrintError(::fmt::sprintf(_("HTTP %ld: %s"), res.status_code, res.body));
    }
    return 1;
  }

  glz::json_t response;
  if (const auto ec = glz::read_json(response, res.body); bool(ec)) {
    fmt.PrintError(
        ::fmt::sprintf(_("Invalid JSON response: %s"), glz::format_error(ec, res.body).c_str()));
    return 1;
  }

  const std::vector<std::string> models = ExtractModelIdsFromJson(response);
  vinput::llm::SaveProviderModels(id, models);

  if (ctx.json_output) {
    const glz::json_t result = {
        {"status", "ok"},
        {"provider", id},
        {"models", vinput::json::FromStrings(models)},
    };
    fmt.PrintJson(result);
    return 0;
  }

  fmt.PrintSuccess(::fmt::sprintf(_("Connected to '%s'. Found %d model(s)."), id,
                                  static_cast<int>(models.size())));
  if (!models.empty()) {
    RenderTestModelsTable(models, fmt);
  }
  return 0;
}
