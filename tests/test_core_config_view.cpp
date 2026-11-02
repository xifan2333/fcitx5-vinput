#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/path_utils.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestLlmLookups() {
  CoreConfig config{};
  LlmProvider p1;
  p1.id = "openai";
  p1.base_url = "https://api.openai.com/v1";
  config.llm.providers.push_back(p1);

  LlmAdapter a1;
  a1.id = "custom-adapter";
  a1.command = "python3";
  config.llm.adapters.push_back(a1);

  const vinput::config::CoreConfigView view(config);

  // Provider
  Expect(view.LlmProvider("") == nullptr, "Empty provider ID must return nullptr");
  Expect(view.LlmProvider("nonexistent") == nullptr, "Nonexistent provider ID must return nullptr");
  const auto* provider = view.LlmProvider("openai");
  Expect(provider != nullptr, "Existing provider ID must be found");
  Expect(provider->base_url == "https://api.openai.com/v1", "Provider base_url must match");

  // Adapter
  Expect(view.LlmAdapter("") == nullptr, "Empty adapter ID must return nullptr");
  Expect(view.LlmAdapter("nonexistent") == nullptr, "Nonexistent adapter ID must return nullptr");
  const auto* adapter = view.LlmAdapter("custom-adapter");
  Expect(adapter != nullptr, "Existing adapter ID must be found");
  Expect(adapter->command == "python3", "Adapter command must match");
}

void TestAsrProviderLookups() {
  CoreConfig config{};

  LocalAsrProvider local1;
  local1.id = "local-sherpa";
  local1.model = "zipformer-zh";
  config.asr.providers.emplace_back(local1);

  CommandAsrProvider cmd1;
  cmd1.id = "cloud-whisper";
  cmd1.command = "whisper-cli";
  config.asr.providers.emplace_back(cmd1);

  const vinput::config::CoreConfigView view(config);

  Expect(view.AsrProvider("") == nullptr, "Empty provider ID must return nullptr");
  Expect(view.AsrProvider("missing") == nullptr, "Missing provider ID must return nullptr");

  const auto* p_local = view.AsrProvider("local-sherpa");
  Expect(p_local != nullptr, "local-sherpa must be found");
  Expect(std::holds_alternative<LocalAsrProvider>(*p_local),
         "local-sherpa must hold LocalAsrProvider");
  Expect(std::get<LocalAsrProvider>(*p_local).model == "zipformer-zh",
         "local model name must match");

  const auto* p_cmd = view.AsrProvider("cloud-whisper");
  Expect(p_cmd != nullptr, "cloud-whisper must be found");
  Expect(std::holds_alternative<CommandAsrProvider>(*p_cmd),
         "cloud-whisper must hold CommandAsrProvider");
  Expect(std::get<CommandAsrProvider>(*p_cmd).command == "whisper-cli", "cloud command must match");
}

void TestActiveAsrProvider() {
  CoreConfig config{};

  LocalAsrProvider local;
  local.id = "sherpa";
  local.model = "model-a";
  config.asr.providers.emplace_back(local);

  CommandAsrProvider cmd;
  cmd.id = "remote";
  cmd.command = "remote.sh";
  config.asr.providers.emplace_back(cmd);

  // No active provider set
  {
    const vinput::config::CoreConfigView view(config);
    Expect(view.ActiveAsrProvider() == nullptr, "Unset active provider must return nullptr");
    Expect(view.ActiveLocalAsrProvider() == nullptr,
           "Unset active local provider must return nullptr");
  }

  // Active provider set to remote
  {
    config.asr.activeProvider = "remote";
    const vinput::config::CoreConfigView view(config);
    Expect(view.ActiveAsrProvider() != nullptr, "Active provider must be found");
    Expect(std::holds_alternative<CommandAsrProvider>(*view.ActiveAsrProvider()),
           "Active provider must be remote command");
    Expect(view.ActiveLocalAsrProvider() == nullptr,
           "Active local provider must return nullptr when active is remote");
  }

  // Active provider set to local
  {
    config.asr.activeProvider = "sherpa";
    const vinput::config::CoreConfigView view(config);
    Expect(view.ActiveAsrProvider() != nullptr, "Active provider must be found");
    Expect(view.ActiveLocalAsrProvider() != nullptr,
           "Active local provider must match when active is local");
    Expect(std::get<LocalAsrProvider>(*view.ActiveLocalAsrProvider()).model == "model-a",
           "Active local provider model must match");
  }
}

void TestPreferredLocalFallbackChain() {
  // Case 1: No local providers at all
  {
    CoreConfig config{};
    CommandAsrProvider cmd;
    cmd.id = "remote-only";
    config.asr.providers.emplace_back(cmd);
    config.asr.activeProvider = "remote-only";

    const vinput::config::CoreConfigView view(config);
    Expect(view.PreferredLocalAsrProvider() == nullptr,
           "PreferredLocalAsrProvider must be nullptr when no local provider exists");
    Expect(view.PreferredLocalModel().empty(),
           "PreferredLocalModel must be empty when no local provider exists");
  }

  // Case 2: Active provider is remote, but local provider exists in list
  {
    CoreConfig config{};
    LocalAsrProvider local1;
    local1.id = "local-first";
    local1.model = "first-model";
    config.asr.providers.emplace_back(local1);

    LocalAsrProvider local2;
    local2.id = "local-second";
    local2.model = "second-model";
    config.asr.providers.emplace_back(local2);

    CommandAsrProvider cmd;
    cmd.id = "remote";
    config.asr.providers.emplace_back(cmd);
    config.asr.activeProvider = "remote";

    const vinput::config::CoreConfigView view(config);
    const auto* preferred = view.PreferredLocalAsrProvider();
    Expect(preferred != nullptr, "Preferred local provider must fall back to first local");
    Expect(AsrProviderId(*preferred) == "local-first",
           "Preferred local provider must be local-first");
    Expect(view.PreferredLocalModel() == "first-model",
           "Preferred local model must be first-model");
  }

  // Case 3: Active provider is local2
  {
    CoreConfig config{};
    LocalAsrProvider local1;
    local1.id = "local-first";
    local1.model = "first-model";
    config.asr.providers.emplace_back(local1);

    LocalAsrProvider local2;
    local2.id = "local-second";
    local2.model = "second-model";
    config.asr.providers.emplace_back(local2);

    config.asr.activeProvider = "local-second";

    const vinput::config::CoreConfigView view(config);
    const auto* preferred = view.PreferredLocalAsrProvider();
    Expect(preferred != nullptr, "Preferred local provider must be found");
    Expect(AsrProviderId(*preferred) == "local-second",
           "Preferred local provider must prioritize active local provider");
    Expect(view.PreferredLocalModel() == "second-model",
           "Preferred local model must match active local model");
  }
}

void TestRegistryUrls() {
  CoreConfig config{};
  config.registry.baseUrls = {
      "https://mirror1.vinput.org/", // with trailing slash
      "https://mirror2.vinput.org",  // without trailing slash
      "",                            // empty, should be skipped
      "https://mirror1.vinput.org",  // duplicate after trimming slash
      "https://mirror1.vinput.org/", // duplicate with trailing slash
  };

  const vinput::config::CoreConfigView view(config);

  const auto model_urls = view.ModelRegistryUrls();
  Expect(model_urls.size() == 2, "Model registry URLs must be deduplicated to 2");
  Expect(model_urls[0] == "https://mirror1.vinput.org/registry/models.json",
         "First model URL mismatch");
  Expect(model_urls[1] == "https://mirror2.vinput.org/registry/models.json",
         "Second model URL mismatch");

  const auto asr_urls = view.AsrProviderRegistryUrls();
  Expect(asr_urls.size() == 2, "ASR registry URLs must be deduplicated to 2");
  Expect(asr_urls[0] == "https://mirror1.vinput.org/registry/providers.json",
         "First ASR provider URL mismatch");

  const auto llm_urls = view.LlmAdapterRegistryUrls();
  Expect(llm_urls.size() == 2, "LLM registry URLs must be deduplicated to 2");
  Expect(llm_urls[0] == "https://mirror1.vinput.org/registry/adapters.json",
         "First LLM adapter URL mismatch");

  const auto i18n_urls = view.RegistryI18nUrls("zh_CN");
  Expect(i18n_urls.size() == 2, "I18n registry URLs must be deduplicated to 2");
  Expect(i18n_urls[0] == "https://mirror1.vinput.org/i18n/zh_CN.json", "First i18n URL mismatch");
  Expect(i18n_urls[1] == "https://mirror2.vinput.org/i18n/zh_CN.json", "Second i18n URL mismatch");
}

void TestCommandScene() {
  CoreConfig config{};

  // No command scene
  {
    vinput::scene::Definition normal_scene;
    normal_scene.id = "chat";
    config.scenes.definitions.push_back(normal_scene);

    const vinput::config::CoreConfigView view(config);
    Expect(view.CommandScene() == nullptr,
           "CommandScene must return nullptr when no command scene is configured");
  }

  // With command scene
  {
    vinput::scene::Definition cmd_scene;
    cmd_scene.id = vinput::scene::kCommandSceneId;
    cmd_scene.label = "Command Mode";
    config.scenes.definitions.push_back(cmd_scene);

    const vinput::config::CoreConfigView view(config);
    const auto* found = view.CommandScene();
    Expect(found != nullptr, "CommandScene must find registered command scene");
    Expect(found->label == "Command Mode", "Command scene label must match");
  }
}

void TestInfrastructureAndPassthrough() {
  CoreConfig config{};
  config.version = ">=2.4.0";
  config.asr.activeProvider = "test-provider";

  const vinput::config::CoreConfigView view(config);
  Expect(&view.Config() == &config, "Config() must return reference to original config object");
  Expect(view.ModelBaseDir() == vinput::path::DefaultModelBaseDir(),
         "ModelBaseDir() must match DefaultModelBaseDir()");
}

} // namespace

int main() {
  TestLlmLookups();
  TestAsrProviderLookups();
  TestActiveAsrProvider();
  TestPreferredLocalFallbackChain();
  TestRegistryUrls();
  TestCommandScene();
  TestInfrastructureAndPassthrough();

  std::cout << "All test_core_config_view tests passed successfully!\n";
  return 0;
}
