#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"

// API Functions
CoreConfig LoadCoreConfig();
bool LoadBundledDefaultCoreConfig(CoreConfig* config, std::string* error);
bool InitializeCoreConfig(std::string* error);
bool SaveCoreConfig(const CoreConfig& config);
std::string GetCoreConfigPath();

// Serializes the config to the on-disk `config.json` representation: four space
// indentation, meaningful fields only.
std::string CoreConfigToJson(const CoreConfig& config);
// Parses the on-disk `config.json` representation. On failure `error` receives a
// diagnostic and `config` is left untouched.
bool CoreConfigFromJson(std::string_view text, CoreConfig* config, std::string* error);

void NormalizeCoreConfig(CoreConfig* config);
bool ValidateCoreConfig(const CoreConfig& config, std::string* error);
const LlmProvider* ResolveLlmProvider(const CoreConfig& config, const std::string& provider_id);
const LlmAdapter* ResolveLlmAdapter(const CoreConfig& config, const std::string& adapter_id);
const AsrProvider* ResolveAsrProvider(const CoreConfig& config, const std::string& provider_id);
const AsrProvider* ResolveActiveAsrProvider(const CoreConfig& config);
const AsrProvider* ResolveActiveLocalAsrProvider(const CoreConfig& config);
const AsrProvider* ResolvePreferredLocalAsrProvider(const CoreConfig& config);
std::string ResolvePreferredLocalModel(const CoreConfig& config);
std::vector<std::string> ResolveModelRegistryUrls(const CoreConfig& config);
std::vector<std::string> ResolveAsrProviderRegistryUrls(const CoreConfig& config);
std::vector<std::string> ResolveLlmAdapterRegistryUrls(const CoreConfig& config);
std::vector<std::string> ResolveRegistryI18nUrls(const CoreConfig& config,
                                                 const std::string& locale);
bool SetPreferredLocalModel(CoreConfig* config, const std::string& model, std::string* error);
const vinput::scene::Definition* FindCommandScene(const CoreConfig& config);
std::filesystem::path ResolveModelBaseDir(const CoreConfig& config);
