#include "cli/command_llm.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "common/i18n.h"
#include "common/core_config.h"

static std::string MaskApiKey(const std::string &key) {
  if (key.size() <= 8)
    return std::string(key.size(), '*');
  return key.substr(0, 4) + std::string(key.size() - 8, '*') +
         key.substr(key.size() - 4);
}

static std::string FormatMsg(const char *tmpl, const std::string &arg) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), tmpl, arg.c_str());
  return buf;
}

int RunLlmList(Formatter &fmt, const CliContext &ctx) {
  auto config = LoadCoreConfig();
  const auto &providers = config.llm.providers;

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &p : providers) {
      arr.push_back({{"name", p.name},
                     {"base_url", p.base_url},
                     {"model", p.model},
                     {"api_key", ""},
                     {"active", p.name == config.llm.activeProvider}});
    }
    fmt.PrintJson(arr);
    return 0;
  }

  std::vector<std::string> headers = {_("NAME"), _("BASE_URL"), _("MODEL"), _("API_KEY")};
  std::vector<std::vector<std::string>> rows;
  for (const auto &p : providers) {
    std::string display_name = p.name;
    if (p.name == config.llm.activeProvider)
      display_name = "[*] " + p.name;
    rows.push_back({display_name, p.base_url, p.model, MaskApiKey(p.api_key)});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunLlmAdd(const std::string &name, const std::string &base_url,
              const std::string &model, const std::string &api_key,
              int timeout_ms, Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  auto config = LoadCoreConfig();
  for (const auto &p : config.llm.providers) {
    if (p.name == name) {
      fmt.PrintError(FormatMsg(_("LLM provider '%s' already exists."), name));
      return 1;
    }
  }

  LlmProvider provider;
  provider.name = name;
  provider.base_url = base_url;
  provider.model = model;
  provider.api_key = api_key;
  provider.timeout_ms = timeout_ms;
  config.llm.providers.push_back(provider);

  // Auto-set active provider if this is the first
  if (config.llm.activeProvider.empty()) {
    config.llm.activeProvider = name;
  }

  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }
  fmt.PrintSuccess(FormatMsg(_("LLM provider '%s' added."), name));
  return 0;
}

int RunLlmUse(const std::string &name, Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  auto config = LoadCoreConfig();
  bool found = false;
  for (const auto &p : config.llm.providers) {
    if (p.name == name) {
      found = true;
      break;
    }
  }
  if (!found) {
    fmt.PrintError(FormatMsg(_("LLM provider '%s' not found."), name));
    return 1;
  }
  config.llm.activeProvider = name;
  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }
  fmt.PrintSuccess(FormatMsg(_("Active LLM provider set to '%s'."), name));
  return 0;
}

int RunLlmRemove(const std::string &name, bool force, Formatter &fmt,
                 const CliContext &ctx) {
  (void)ctx;
  auto config = LoadCoreConfig();
  if (name == config.llm.activeProvider && !force) {
    fmt.PrintError(FormatMsg(_("Cannot remove active provider '%s'. Use --force."), name));
    return 1;
  }
  auto &providers = config.llm.providers;
  auto it =
      std::find_if(providers.begin(), providers.end(),
                   [&name](const LlmProvider &p) { return p.name == name; });
  if (it == providers.end()) {
    fmt.PrintError(FormatMsg(_("LLM provider '%s' not found."), name));
    return 1;
  }
  providers.erase(it);
  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }
  fmt.PrintSuccess(FormatMsg(_("LLM provider '%s' removed."), name));
  return 0;
}

int RunLlmEnable(Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  auto config = LoadCoreConfig();
  if (config.llm.enabled) {
    fmt.PrintInfo(_("LLM is already enabled."));
    return 0;
  }
  config.llm.enabled = true;
  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }
  fmt.PrintSuccess(_("LLM features are now ENABLED. Restart the daemon to apply."));
  return 0;
}

int RunLlmDisable(Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  auto config = LoadCoreConfig();
  if (!config.llm.enabled) {
    fmt.PrintInfo(_("LLM is already disabled."));
    return 0;
  }
  config.llm.enabled = false;
  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }
  fmt.PrintSuccess(_("LLM features are now DISABLED. Restart the daemon to apply."));
  return 0;
}
