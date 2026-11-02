#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"

namespace vinput::config {

class CoreConfigView {
public:
  explicit CoreConfigView(const CoreConfig& config) noexcept : config_(&config) {}
  explicit CoreConfigView(const CoreConfig* config) noexcept : config_(config) {}

  // ASR derivation and fallback chain
  [[nodiscard]] const ::AsrProvider* ActiveAsrProvider() const noexcept;
  [[nodiscard]] const ::AsrProvider* ActiveLocalAsrProvider() const noexcept;
  [[nodiscard]] const ::AsrProvider* PreferredLocalAsrProvider() const noexcept;
  [[nodiscard]] std::string PreferredLocalModel() const;
  [[nodiscard]] const ::AsrProvider* AsrProvider(std::string_view provider_id) const noexcept;

  // LLM providers & adapters
  [[nodiscard]] const ::LlmProvider* LlmProvider(std::string_view provider_id) const noexcept;
  [[nodiscard]] const ::LlmAdapter* LlmAdapter(std::string_view adapter_id) const noexcept;

  // Scene definitions
  [[nodiscard]] const vinput::scene::Definition* CommandScene() const noexcept;

  // Registry URLs
  [[nodiscard]] std::vector<std::string> ModelRegistryUrls() const;
  [[nodiscard]] std::vector<std::string> AsrProviderRegistryUrls() const;
  [[nodiscard]] std::vector<std::string> LlmAdapterRegistryUrls() const;
  [[nodiscard]] std::vector<std::string> RegistryI18nUrls(std::string_view locale) const;

  // Infrastructure paths
  [[nodiscard]] static std::filesystem::path ModelBaseDir();

  // Underlying config reference
  [[nodiscard]] const CoreConfig& Config() const noexcept { return *config_; }

private:
  const CoreConfig* config_{nullptr};
};

} // namespace vinput::config

using CoreConfigView = vinput::config::CoreConfigView;
