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
  PostprocessRequest req;
  req.text = "hello";
  const auto res = pipeline.Process(req, config);
  Expect(!res.errors.empty(), "Uninitialized pipeline must return error");
  Expect(res.payload.commitText.empty(), "Uninitialized pipeline must return empty payload");
}

void TestEmptyText() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);
  const CoreConfig config{};
  PostprocessRequest req;
  req.text = "";
  const auto res = pipeline.Process(req, config);
  Expect(res.errors.empty(), "Empty text must not produce error");
  Expect(res.payload.commitText.empty(), "Empty text must produce empty payload");
}

void TestPlainSceneNoLlm() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition scene;
  scene.id = "plain_scene";
  scene.llm_max_candidates = 0; // No LLM configured
  config.scenes.definitions.push_back(scene);
  config.scenes.activeScene = "plain_scene";

  bool entered_postprocessing = false;
  PostprocessRequest req;
  req.text = "raw transcript";
  req.scene_id = "plain_scene";
  req.is_command = false;

  const auto res =
      pipeline.Process(req, config, [&entered_postprocessing]() { entered_postprocessing = true; });

  Expect(!entered_postprocessing, "Non-LLM scene must not invoke on_enter_postprocessing");
  Expect(res.errors.empty(), "No errors expected");
  Expect(res.payload.commitText == "raw transcript", "Raw transcript must be preserved directly");
}

void TestExplicitSceneWithLlm() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition scene;
  scene.id = "ai_scene";
  scene.provider_id = "openai";
  scene.llm_max_candidates = 1;
  config.scenes.definitions.push_back(scene);

  bool entered_postprocessing = false;
  PostprocessRequest req;
  req.text = "prompt text";
  req.scene_id = "ai_scene";
  req.is_command = false;

  const auto res =
      pipeline.Process(req, config, [&entered_postprocessing]() { entered_postprocessing = true; });

  Expect(entered_postprocessing, "Explicit LLM scene must invoke on_enter_postprocessing");
  Expect(!res.payload.commitText.empty(), "Payload commitText should not be empty");
}

void TestDefaultActiveSceneOmitted() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition scene;
  scene.id = "default_ai";
  scene.provider_id = "openai";
  scene.llm_max_candidates = 1;
  config.scenes.definitions.push_back(scene);
  config.scenes.activeScene = "default_ai";

  bool entered_postprocessing = false;
  PostprocessRequest req;
  req.text = "prompt text";
  req.scene_id = ""; // Empty scene_id, should use activeScene
  req.is_command = false;

  const auto res =
      pipeline.Process(req, config, [&entered_postprocessing]() { entered_postprocessing = true; });

  Expect(entered_postprocessing, "Omitted scene_id must use activeScene and invoke LLM");
  Expect(!res.payload.commitText.empty(), "Payload commitText should not be empty");
}

void TestCommandMode() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition cmd_scene;
  cmd_scene.id = std::string(vinput::scene::kCommandSceneId);
  cmd_scene.provider_id = "command_llm";
  cmd_scene.llm_max_candidates = 1;
  config.scenes.definitions.push_back(cmd_scene);

  bool entered_postprocessing = false;
  PostprocessRequest req;
  req.text = "search for something";
  req.scene_id = "";
  req.is_command = true;
  req.selected_text = "some context";

  const auto res =
      pipeline.Process(req, config, [&entered_postprocessing]() { entered_postprocessing = true; });

  Expect(entered_postprocessing, "Command scene with LLM must invoke on_enter_postprocessing");
  Expect(!res.payload.commitText.empty(), "Command payload commitText should not be empty");
}

void TestNonExistentSceneFailsHonestly() {
  PostProcessor processor;
  const RecognitionPipeline pipeline(&processor);

  CoreConfig config{};
  vinput::scene::Definition active_scene;
  active_scene.id = "active_default";
  active_scene.provider_id = "openai";
  active_scene.llm_max_candidates = 1;
  config.scenes.definitions.push_back(active_scene);
  config.scenes.activeScene = "active_default";

  bool entered_postprocessing = false;
  PostprocessRequest req;
  req.text = "untranslated speech";
  req.scene_id = "non_existent_scene"; // Specified explicitly, but does not exist!
  req.is_command = false;

  const auto res =
      pipeline.Process(req, config, [&entered_postprocessing]() { entered_postprocessing = true; });

  // Must NOT silently substitute active_default and run the wrong prompt!
  Expect(!entered_postprocessing, "Must NOT enter postprocessing on non-existent scene");
  Expect(!res.errors.empty(), "Must honestly report that the requested scene was not found");
  Expect(res.payload.commitText == "untranslated speech", "Raw text must be preserved for user");
}

} // namespace

int main() {
  TestUninitializedPipeline();
  TestEmptyText();
  TestPlainSceneNoLlm();
  TestExplicitSceneWithLlm();
  TestDefaultActiveSceneOmitted();
  TestCommandMode();
  TestNonExistentSceneFailsHonestly();

  std::cout << "All RecognitionPipeline tests passed!\n";
  return 0;
}
