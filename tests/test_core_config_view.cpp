#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/config/core_config.h"
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
  Expect(vinput::config::CoreConfigView::ModelBaseDir() == vinput::path::DefaultModelBaseDir(),
         "ModelBaseDir() must match DefaultModelBaseDir()");
}

void TestSceneValidation() {
  std::string error;

  // Case 1: valid adapter scene (has provider_id, empty model)
  vinput::scene::Definition adapter_scene;
  adapter_scene.id = "translate_adapter_scene";
  adapter_scene.provider_id = "my-adapter";
  adapter_scene.model.clear();
  Expect(vinput::scene::ValidateDefinition(adapter_scene, &error),
         "Scene with provider_id but empty model (adapter) must be valid");
  Expect(error.empty(), "Error string must be empty for valid adapter scene");

  // Case 2: valid LLM provider scene (has provider_id and model)
  vinput::scene::Definition llm_scene;
  llm_scene.id = "deepseek_scene";
  llm_scene.provider_id = "deepseek";
  llm_scene.model = "deepseek-chat";
  Expect(vinput::scene::ValidateDefinition(llm_scene, &error),
         "Scene with both provider_id and model must be valid");

  // Case 3: valid raw/passthrough scene (empty provider_id and empty model)
  vinput::scene::Definition raw_scene;
  raw_scene.id = "raw_scene";
  Expect(vinput::scene::ValidateDefinition(raw_scene, &error),
         "Scene with neither provider_id nor model must be valid");

  // Case 4: invalid scene (has model but empty provider_id)
  vinput::scene::Definition invalid_scene;
  invalid_scene.id = "invalid_scene";
  invalid_scene.model = "gpt-4o";
  Expect(!vinput::scene::ValidateDefinition(invalid_scene, &error),
         "Scene with model but no provider_id must be invalid");
  Expect(error == "Scene model cannot be configured without a provider.",
         "Error message must specify model cannot be configured without a provider");

  // Case 5: CoreConfig validation: LLM provider requires model, adapter permits empty model
  CoreConfig cfg;
  cfg.version = ">=2.4.0";
  vinput::scene::Definition s_raw;
  s_raw.id = std::string(vinput::scene::kRawSceneId);
  cfg.scenes.definitions.push_back(s_raw);

  vinput::scene::Definition s_cmd;
  s_cmd.id = std::string(vinput::scene::kCommandSceneId);
  cfg.scenes.definitions.push_back(s_cmd);

  LlmProvider test_prov;
  test_prov.id = "my-llm";
  cfg.llm.providers.push_back(test_prov);

  LlmAdapter test_adap;
  test_adap.id = "my-adapter";
  cfg.llm.adapters.push_back(test_adap);

  // Valid with adapter and empty model
  vinput::scene::Definition s_adapter;
  s_adapter.id = "adapter_scene";
  s_adapter.provider_id = "my-adapter";
  cfg.scenes.definitions.push_back(s_adapter);
  Expect(ValidateCoreConfig(cfg, &error), "CoreConfig with adapter and empty model must be valid");

  // Invalid with LLM provider and empty model
  vinput::scene::Definition s_llm;
  s_llm.id = "llm_empty_model";
  s_llm.provider_id = "my-llm";
  cfg.scenes.definitions.push_back(s_llm);
  Expect(!ValidateCoreConfig(cfg, &error),
         "CoreConfig with LLM provider and empty model must be invalid");
  Expect(error.find("Scene model must be configured for LLM providers") != std::string::npos,
         "Error must reject empty model for LLM provider");
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
  TestSceneValidation();

  std::cout << "All test_core_config_view tests passed successfully!\n";
  return 0;
}
