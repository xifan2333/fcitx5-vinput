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
bool SetPreferredLocalModel(CoreConfig* config, const std::string& model, std::string* error);
