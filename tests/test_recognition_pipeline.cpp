#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/scene/postprocess_scene.h"

#include "daemon/postprocess/post_processor.h"
#include "daemon/runtime/recognition_pipeline.h"

namespace {

using vinput::daemon::runtime::PostprocessRequest;
using vinput::daemon::runtime::RecognitionPipeline;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestUninitializedPipeline() {
  const RecognitionPipeline pipeline(nullptr);
  const CoreConfig config{};
  const PostprocessRequest req{.text = "hello"};
  const auto res = pipeline.Process(req, config);
  Expect(!res.errors.empty(), "Uninitialized pipeline must return error");
  Expect(res.payload.commitText.empty(), "Uninitialized pipeline must return empty payload");
}

void TestEmptyText() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);
  const CoreConfig config{};
  const PostprocessRequest req{.text = ""};
  const auto res = pipeline.Process(req, config);
  Expect(res.errors.empty(), "Empty text must not produce error");
  Expect(res.payload.commitText.empty(), "Empty text must produce empty payload");
}

void TestNormalDictationWithoutLlm() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition scene;
  scene.id = "plain_scene";
  scene.llm_max_candidates = 0; // No LLM
  config.scenes.definitions.push_back(scene);
  config.scenes.activeScene = "plain_scene";

  bool entered_postprocessing = false;
  const PostprocessRequest req{
      .text = "raw transcript",
      .scene_id = "plain_scene",
      .is_command = false,
      .selected_text = {},
  };

  const auto res =
      pipeline.Process(req, config, [&entered_postprocessing]() { entered_postprocessing = true; });

  Expect(!entered_postprocessing, "Non-LLM scene must not invoke on_enter_postprocessing");
  Expect(res.errors.empty(), "No errors expected");
  Expect(res.payload.commitText == "raw transcript", "Raw transcript must be preserved");
}

void TestDictationWithLlmCallback() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition scene;
  scene.id = "ai_scene";
  scene.provider_id = "openai";
  scene.llm_max_candidates = 1;
  config.scenes.definitions.push_back(scene);
  config.scenes.activeScene = "ai_scene";

  bool entered_postprocessing = false;
  const PostprocessRequest req{
      .text = "prompt text",
      .scene_id = "ai_scene",
      .is_command = false,
      .selected_text = {},
  };

  // With provider_id set but no valid server running, on_enter_postprocessing must still be
  // triggered!
  const auto res =
      pipeline.Process(req, config, [&entered_postprocessing]() { entered_postprocessing = true; });

  Expect(entered_postprocessing, "LLM scene must invoke on_enter_postprocessing");
  Expect(!res.payload.commitText.empty(), "Result payload commitText should not be empty");
}

void TestCommandModeResolution() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition cmd_scene;
  cmd_scene.id = std::string(vinput::scene::kCommandSceneId);
  cmd_scene.provider_id = "command_llm";
  cmd_scene.llm_max_candidates = 1;
  config.scenes.definitions.push_back(cmd_scene);

  bool entered_postprocessing = false;
  const PostprocessRequest req{
      .text = "search for something",
      .scene_id = {},
      .is_command = true,
      .selected_text = "some context",
  };

  const auto res =
      pipeline.Process(req, config, [&entered_postprocessing]() { entered_postprocessing = true; });

  Expect(entered_postprocessing, "Command scene with LLM must invoke on_enter_postprocessing");
  Expect(!res.payload.commitText.empty(), "Command result payload commitText should not be empty");
}

void TestFallbackSceneResolution() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition default_scene;
  default_scene.id = "active_default";
  default_scene.llm_max_candidates = 0;
  config.scenes.definitions.push_back(default_scene);
  config.scenes.activeScene = "active_default";

  // Request a non-existent scene ID
  const PostprocessRequest req{
      .text = "fallback test",
      .scene_id = "non_existent_scene",
      .is_command = false,
      .selected_text = {},
  };

  const auto res = pipeline.Process(req, config);
  Expect(res.errors.empty(), "Fallback scene resolution must succeed without error");
  Expect(res.payload.commitText == "fallback test", "Result text must be returned");
}

} // namespace

int main() {
  TestUninitializedPipeline();
  TestEmptyText();
  TestNormalDictationWithoutLlm();
  TestDictationWithLlmCallback();
  TestCommandModeResolution();
  TestFallbackSceneResolution();

  std::cout << "All RecognitionPipeline tests passed!\n";
  return 0;
}
