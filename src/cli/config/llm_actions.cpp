#include "cli/config/llm_actions.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include "cli/utils/cli_helpers.h"
#include "cli/utils/resource_utils.h"
#include "cli/runtime/dbus_client.h"
#include "common/config/core_config.h"
#include "common/i18n.h"
#include "common/llm/adapter_manager.h"
#include "common/llm/defaults.h"
#include "common/registry/registry_i18n.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/string_utils.h"

namespace {

std::string MaskApiKey(const std::string &key) {
  if (key.size() <= 8) {
    return std::string(key.size(), '*');
  }
  return key.substr(0, 4) + std::string(key.size() - 8, '*') +
         key.substr(key.size() - 4);
}

bool ParseExtraBody(const std::string &raw, nlohmann::json *out,
                    Formatter &fmt) {
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
  } catch (const std::exception &e) {
    fmt.PrintError(vinput::str::FmtStr(_("Invalid --extra-body JSON: %s"),
                                       e.what()));
    return false;
  }
}

}  // namespace

int RunLlmConfigList(Formatter &fmt, const CliContext &ctx) {
  CoreConfig config = LoadCoreConfig();

  if (ctx.json_output) {
    nlohmann::json providers = nlohmann::json::array();
    for (const auto &provider : config.llm.providers) {
      providers.push_back({
          {"id", provider.id},
          {"base_url", provider.base_url},
          {"api_key", ""},
          {"extra_body", provider.extra_body.is_object() ? provider.extra_body
                                                         : nlohmann::json::object()},
      });
    }
    fmt.PrintJson(providers);
    return 0;
  }

  std::vector<std::string> headers = {_("ID"), _("BASE_URL"), _("API_KEY")};
  std::vector<std::vector<std::string>> rows;
  for (const auto &provider : config.llm.providers) {
    rows.push_back({provider.id, provider.base_url, MaskApiKey(provider.api_key)});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunLlmConfigListAdapters(bool available, Formatter &fmt,
                             const CliContext &ctx) {
  CoreConfig config = LoadCoreConfig();
  const auto installed_display_map =
      vinput::cli::FetchScriptDisplayMap(config, vinput::script::Kind::kLlmAdapter);

  if (!available) {
    if (ctx.json_output) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto &adapter : config.llm.adapters) {
        const auto it = installed_display_map.find(adapter.id);
        arr.push_back({
            {"id", vinput::cli::HumanizeResourceId(installed_display_map,
                                                   adapter.id)},
            {"machine_id", adapter.id},
            {"title", it == installed_display_map.end() ? "" : it->second.title},
            {"readme_url",
             it == installed_display_map.end() ? "" : it->second.readme_url},
            {"command", adapter.command},
            {"args", adapter.args},
            {"env", adapter.env},
            {"running", vinput::adapter::IsRunning(adapter.id)},
        });
      }
      fmt.PrintJson(arr);
      return 0;
    }

    std::vector<std::string> headers = {_("ID"), _("TITLE"),
                                        _("README")};
    std::vector<std::vector<std::string>> rows;
    for (const auto &adapter : config.llm.adapters) {
      rows.push_back({vinput::cli::HumanizeResourceId(installed_display_map,
                                                     adapter.id),
                      installed_display_map.count(adapter.id) == 0
                          ? ""
                          : installed_display_map.at(adapter.id).title,
                      installed_display_map.count(adapter.id) == 0
                          ? ""
                          : vinput::cli::FormatTerminalLink(
                                ctx, _("Open README"),
                                installed_display_map.at(adapter.id).readme_url)});
    }
    fmt.PrintTable(headers, rows);
    return 0;
  }

  const auto registryUrls = ResolveLlmAdapterRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(
        _("No LLM adapter registry base URLs configured. Edit config.json and set registry.base_urls."));
    return 1;
  }

  std::string error;
  const auto entries = vinput::script::FetchRegistry(
      config, vinput::script::Kind::kLlmAdapter, registryUrls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto locale = vinput::registry::DetectPreferredLocale();
  const auto i18nMap = vinput::registry::FetchMergedI18nMap(config, locale);
  const auto display_map = vinput::cli::BuildScriptDisplayMap(entries, i18nMap);

  auto isInstalled = [&config](const std::string &id) {
    return ResolveLlmAdapter(config, id) != nullptr;
  };

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &entry : entries) {
      nlohmann::json envs = nlohmann::json::array();
      for (const auto &env : entry.envs) {
        envs.push_back({{"name", env.name}, {"required", env.required}});
      }
      arr.push_back({
          {"id", vinput::cli::HumanizeResourceId(entry.id, entry.short_id)},
          {"machine_id", entry.id},
          {"title", display_map.at(entry.id).title},
          {"description", display_map.at(entry.id).description},
          {"readme_url", entry.readme_url},
          {"envs", envs},
          {"status", isInstalled(entry.id) ? "installed" : "available"},
      });
    }
    fmt.PrintJson(arr);
    return 0;
  }

  std::vector<std::string> headers = {_("ID"), _("TITLE"),
                                      _("STATUS"), _("README")};
  std::vector<std::vector<std::string>> rows;
  for (const auto &entry : entries) {
    rows.push_back({vinput::cli::HumanizeResourceId(entry.id, entry.short_id),
                    display_map.at(entry.id).title,
                    isInstalled(entry.id) ? _("installed") : _("available"),
                    vinput::cli::FormatTerminalLink(ctx, _("Open README"),
                                                    entry.readme_url)});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunLlmConfigAdd(const std::string &id, const std::string &baseUrl,
                    const std::string &apiKey, const std::string &extraBody,
                    Formatter &fmt, const CliContext &ctx) {
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
  provider.id = id;
  provider.base_url = baseUrl;
  provider.api_key = vinput::str::TrimAsciiWhitespace(apiKey);
  provider.extra_body = std::move(extra_json);
  config.llm.providers.push_back(std::move(provider));

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("LLM provider '%s' added."), id));
  return 0;
}

int RunLlmConfigInstallAdapter(const std::string &selector, Formatter &fmt,
                               const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  NormalizeCoreConfig(&config);

  const auto registryUrls = ResolveLlmAdapterRegistryUrls(config);
  if (registryUrls.empty()) {
    fmt.PrintError(
        _("No LLM adapter registry base URLs configured. Edit config.json and set registry.base_urls."));
    return 1;
  }

  std::string error;
  const auto entries = vinput::script::FetchRegistry(
      config, vinput::script::Kind::kLlmAdapter, registryUrls, &error);
  if (!error.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const std::string id = vinput::cli::ResolveScriptSelectorByShortId(
      selector, entries, "LLM adapter", &error);
  if (id.empty()) {
    fmt.PrintError(error);
    return 1;
  }

  const auto it =
      std::find_if(entries.begin(), entries.end(),
                   [&id](const vinput::script::RegistryEntry &entry) {
                     return entry.id == id;
                   });
  if (it == entries.end()) {
    fmt.PrintError(
        vinput::str::FmtStr(_("Adapter '%s' not found in registry."), id));
    return 1;
  }

  std::filesystem::path scriptPath;
  if (!vinput::script::DownloadScript(*it, vinput::script::Kind::kLlmAdapter,
                                      &scriptPath, &error)) {
    fmt.PrintError(error);
    return 1;
  }
  if (!vinput::script::MaterializeLlmAdapter(&config, *it, scriptPath,
                                             &error)) {
    fmt.PrintError(error);
    return 1;
  }
  NormalizeCoreConfig(&config);
  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }

  fmt.PrintSuccess(vinput::str::FmtStr(_("Adapter '%s' added."), selector));
  return 0;
}

int RunLlmConfigStartAdapter(const std::string &id, Formatter &fmt,
                             const CliContext &ctx) {
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

int RunLlmConfigStopAdapter(const std::string &id, Formatter &fmt,
                            const CliContext &ctx) {
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

int RunLlmConfigRemove(const std::string &id, Formatter &fmt,
                       const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  auto &providers = config.llm.providers;
  const auto it = std::find_if(
      providers.begin(), providers.end(),
      [&id](const LlmProvider &provider) { return provider.id == id; });
  if (it == providers.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("LLM provider '%s' not found."), id));
    return 1;
  }

  providers.erase(it);
  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }
  fmt.PrintSuccess(vinput::str::FmtStr(_("LLM provider '%s' removed."), id));
  return 0;
}

int RunLlmConfigEdit(const std::string &id, const std::string &baseUrl,
                     const std::string &apiKey, const std::string &extraBody,
                     bool hasBaseUrl, bool hasApiKey, bool hasExtraBody,
                     Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  auto &providers = config.llm.providers;
  const auto it = std::find_if(
      providers.begin(), providers.end(),
      [&id](const LlmProvider &provider) { return provider.id == id; });
  if (it == providers.end()) {
    fmt.PrintError(vinput::str::FmtStr(_("LLM provider '%s' not found."), id));
    return 1;
  }

  if (hasBaseUrl) {
    it->base_url = baseUrl;
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

size_t CurlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  const size_t total = size * nmemb;
  if (!userdata || !ptr || total == 0) return 0;
  auto *buf = static_cast<std::string *>(userdata);
  if (buf->size() + total > 1 * 1024 * 1024) return 0;
  buf->append(ptr, total);
  return total;
}

}  // namespace

int RunLlmConfigTest(const std::string &id, Formatter &fmt,
                     const CliContext &ctx) {
  CoreConfig config = LoadCoreConfig();
  const auto *provider = ResolveLlmProvider(config, id);
  if (!provider) {
    fmt.PrintError(vinput::str::FmtStr(_("LLM provider '%s' not found."), id));
    return 1;
  }

  if (provider->base_url.empty()) {
    fmt.PrintError(_("Provider base_url is empty."));
    return 1;
  }

  std::string url = provider->base_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += vinput::llm::kOpenAiModelsPath;

  CURL *curl = curl_easy_init();
  if (!curl) {
    fmt.PrintError(_("Failed to initialize libcurl."));
    return 1;
  }

  struct curl_slist *headers = nullptr;
  if (!provider->api_key.empty()) {
    std::string auth = std::string(vinput::llm::kAuthorizationHeader) + ": " +
                       vinput::llm::kBearerPrefix + provider->api_key;
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

  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    fmt.PrintError(vinput::str::FmtStr(_("Connection failed: %s"),
                                       curl_easy_strerror(code)));
    return 1;
  }

  if (status_code < 200 || status_code >= 300) {
    fmt.PrintError(vinput::str::FmtStr(_("HTTP %ld: %s"),
                                       status_code, response_body));
    return 1;
  }

  nlohmann::json response;
  try {
    response = nlohmann::json::parse(response_body);
  } catch (const std::exception &e) {
    fmt.PrintError(vinput::str::FmtStr(_("Invalid JSON response: %s"), e.what()));
    return 1;
  }

  std::vector<std::string> models;
  if (response.contains("data") && response["data"].is_array()) {
    for (const auto &item : response["data"]) {
      if (item.contains("id") && item["id"].is_string()) {
        models.push_back(item["id"].get<std::string>());
      }
    }
  }

  if (ctx.json_output) {
    nlohmann::json result = {
        {"status", "ok"},
        {"provider", id},
        {"models", models},
    };
    fmt.PrintJson(result);
    return 0;
  }

  fmt.PrintSuccess(vinput::str::FmtStr(
      _("Connected to '%s'. Found %d model(s)."), id, (int)models.size()));
  if (!models.empty()) {
    std::sort(models.begin(), models.end());
    std::vector<std::string> headers_row = {_("MODEL")};
    std::vector<std::vector<std::string>> rows;
    for (const auto &m : models) {
      rows.push_back({m});
    }
    fmt.PrintTable(headers_row, rows);
  }
  return 0;
}
