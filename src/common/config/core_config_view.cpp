#include "common/config/core_config_view.h"

#include <filesystem>
#include <fmt/core.h>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/path_utils.h"

namespace vinput::config {

namespace {

std::vector<std::string> ResolveRegistryUrlsForPath(const CoreConfig& config,
                                                    std::string_view suffix) {
  std::vector<std::string> urls;
  std::set<std::string> seen;
  for (const auto& baseUrl : config.registry.baseUrls) {
    if (baseUrl.empty()) {
      continue;
    }
    std::string url = baseUrl;
    while (!url.empty() && url.back() == '/') {
      url.pop_back();
    }
    url += '/';
    url += suffix;
    if (seen.insert(url).second) {
      urls.push_back(std::move(url));
    }
  }
  return urls;
}

} // namespace

const ::LlmProvider* CoreConfigView::LlmProvider(std::string_view provider_id) const noexcept {
  if (provider_id.empty()) {
    return nullptr;
  }
  for (const auto& provider : config_->llm.providers) {
    if (provider.id == provider_id) {
      return &provider;
    }
  }
  return nullptr;
}

const ::LlmAdapter* CoreConfigView::LlmAdapter(std::string_view adapter_id) const noexcept {
  if (adapter_id.empty()) {
    return nullptr;
  }
  for (const auto& adapter : config_->llm.adapters) {
    if (adapter.id == adapter_id) {
      return &adapter;
    }
  }
  return nullptr;
}

const ::AsrProvider* CoreConfigView::AsrProvider(std::string_view provider_id) const noexcept {
  if (provider_id.empty()) {
    return nullptr;
  }
  for (const auto& provider : config_->asr.providers) {
    if (AsrProviderId(provider) == provider_id) {
      return &provider;
    }
  }
  return nullptr;
}

const ::AsrProvider* CoreConfigView::ActiveAsrProvider() const noexcept {
  return AsrProvider(config_->asr.activeProvider);
}

const ::AsrProvider* CoreConfigView::ActiveLocalAsrProvider() const noexcept {
  const ::AsrProvider* provider = ActiveAsrProvider();
  if (provider == nullptr || !std::holds_alternative<LocalAsrProvider>(*provider)) {
    return nullptr;
  }
  return provider;
}

const ::AsrProvider* CoreConfigView::PreferredLocalAsrProvider() const noexcept {
  const ::AsrProvider* active_local = ActiveLocalAsrProvider();
  if (active_local != nullptr) {
    return active_local;
  }
  for (const auto& provider : config_->asr.providers) {
    if (std::holds_alternative<LocalAsrProvider>(provider)) {
      return &provider;
    }
  }
  return nullptr;
}

std::string CoreConfigView::PreferredLocalModel() const {
  const ::AsrProvider* provider = PreferredLocalAsrProvider();
  if (provider == nullptr) {
    return {};
  }
  return std::get<LocalAsrProvider>(*provider).model;
}

std::vector<std::string> CoreConfigView::ModelRegistryUrls() const {
  return ResolveRegistryUrlsForPath(*config_, "registry/models.json");
}

std::vector<std::string> CoreConfigView::AsrProviderRegistryUrls() const {
  return ResolveRegistryUrlsForPath(*config_, "registry/providers.json");
}

std::vector<std::string> CoreConfigView::LlmAdapterRegistryUrls() const {
  return ResolveRegistryUrlsForPath(*config_, "registry/adapters.json");
}

std::vector<std::string> CoreConfigView::RegistryI18nUrls(std::string_view locale) const {
  return ResolveRegistryUrlsForPath(*config_, fmt::format("i18n/{}.json", locale));
}

const vinput::scene::Definition* CoreConfigView::CommandScene() const noexcept {
  for (const auto& scene : config_->scenes.definitions) {
    if (scene.id == vinput::scene::kCommandSceneId) {
      return &scene;
    }
  }
  return nullptr;
}

std::filesystem::path CoreConfigView::ModelBaseDir() {
  return vinput::path::DefaultModelBaseDir();
}

} // namespace vinput::config
