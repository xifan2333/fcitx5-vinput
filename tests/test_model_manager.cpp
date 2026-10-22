#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>

#include "common/asr/model_manager.h"
#include "common/utils/expected.h"
#include "common/utils/file_utils.h"

namespace fs = std::filesystem;

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

// RAII guard for a unique temporary model base directory.
struct TempBaseDir {
  std::unique_ptr<vinput::file::ScopedTempDir> holder;
  fs::path path;

  TempBaseDir() {
    std::string error;
    holder = vinput::file::ScopedTempDir::Create("", ".tmp-vinput-model-manager-", &error);
    TEST_ASSERT(holder != nullptr);
    // Canonicalize so that symlinked temp roots (e.g. /tmp on some distros)
    // match the canonical paths produced by model metadata resolution.
    path = fs::weakly_canonical(holder->Path());
  }

  ~TempBaseDir() = default;

  TempBaseDir(const TempBaseDir&) = delete;
  TempBaseDir& operator=(const TempBaseDir&) = delete;
  TempBaseDir(TempBaseDir&&) = delete;
  TempBaseDir& operator=(TempBaseDir&&) = delete;
};

void WriteFile(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();
  TEST_ASSERT(fs::exists(path));
}

// Creates base/<engine>/<name>/ with a valid vinput-model.json, tokens and a
// model weight file. Returns the model id (model.<engine>.<name>).
std::string CreateValidModel(const fs::path& base, const std::string& engine,
                             const std::string& name) {
  const fs::path dir = base / engine / name;
  fs::create_directories(dir);
  WriteFile(dir / "tokens.txt", "<blk> 0\n");
  WriteFile(dir / "encoder.onnx", "fake-onnx-weights");
  WriteFile(dir / "vinput-model.json",
            R"json({
  "backend": "sherpa-offline",
  "runtime": "offline",
  "family": "transducer",
  "language": "zh",
  "supports_hotwords": true,
  "size_bytes": 42,
  "model": {
    "tokens": "tokens.txt",
    "transducer": {"encoder": "encoder.onnx", "decoder": "decoder.onnx", "joiner": "joiner.onnx"}
  }
})json");
  return "model." + engine + "." + name;
}

void TestModelIdPathMapping() {
  TEST_ASSERT(ModelManager::RelativePathForId("model.sherpa.zipformer") ==
              fs::path("sherpa") / "zipformer");
  TEST_ASSERT(ModelManager::RelativePathForId("model.a.b.c") == fs::path("a") / "b.c");
  TEST_ASSERT(ModelManager::RelativePathForId("").empty());
  TEST_ASSERT(ModelManager::RelativePathForId("model").empty());
  TEST_ASSERT(ModelManager::RelativePathForId("model.only").empty());
  TEST_ASSERT(ModelManager::RelativePathForId("model.a..b").empty());
  TEST_ASSERT(ModelManager::RelativePathForId("model/a/b").empty());

  TEST_ASSERT(ModelManager::IdFromRelativePath(fs::path("sherpa") / "zipformer") ==
              "model.sherpa.zipformer");
  TEST_ASSERT(ModelManager::IdFromRelativePath(fs::path("sherpa")).empty());
  TEST_ASSERT(ModelManager::IdFromRelativePath(fs::path("a") / "b" / "c").empty());
  TEST_ASSERT(ModelManager::IdFromRelativePath(fs::path("..") / "b").empty());
}

void TestModelDirComposition() {
  const TempBaseDir temp;
  const ModelManager manager(temp.path.string(), "model.sherpa.zipformer");
  TEST_ASSERT(manager.ModelDir("model.sherpa.zipformer") == temp.path / "sherpa" / "zipformer");
  TEST_ASSERT(manager.ModelDir("invalid id").empty());
  TEST_ASSERT(manager.GetBaseDir() == temp.path.string());
  TEST_ASSERT(manager.GetModelId() == "model.sherpa.zipformer");
}

void TestEnsureModelsAndGetModelInfoSuccess() {
  const TempBaseDir temp;
  const std::string id = CreateValidModel(temp.path, "sherpa", "zipformer");

  const ModelManager manager(temp.path.string(), id);
  const auto ensure = manager.EnsureModels();
  TEST_ASSERT(ensure.has_value());

  auto info = manager.GetModelInfo();
  TEST_ASSERT(info.has_value());
  TEST_ASSERT(info->backend == "sherpa-offline");
  TEST_ASSERT(info->runtime == "offline");
  TEST_ASSERT(info->family == "transducer");
  TEST_ASSERT(info->model_type == "transducer");
  TEST_ASSERT(info->language == "zh");
  TEST_ASSERT(info->supports_hotwords);
  TEST_ASSERT(info->size_bytes == 42);
  TEST_ASSERT(info->rejected_files.empty());
  TEST_ASSERT(info->File("tokens") == (temp.path / "sherpa" / "zipformer" / "tokens.txt").string());
  TEST_ASSERT(info->File("encoder") ==
              (temp.path / "sherpa" / "zipformer" / "encoder.onnx").string());

  // Monadic chaining over the Expected result.
  auto family_upper = manager.GetModelInfo().transform([](const ModelInfo& m) {
    std::string family = m.family;
    for (auto& c : family) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return family;
  });
  TEST_ASSERT(family_upper.has_value());
  TEST_ASSERT(*family_upper == "TRANSDUCER");
}

void TestEnsureModelsMissingMetadata() {
  const TempBaseDir temp;
  const ModelManager manager(temp.path.string(), "model.sherpa.missing");

  const auto ensure = manager.EnsureModels();
  TEST_ASSERT(!ensure.has_value());
  TEST_ASSERT(ensure.error().find("vinput-model.json") != std::string::npos);

  const auto info = manager.GetModelInfo();
  TEST_ASSERT(!info.has_value());
  TEST_ASSERT(info.error().find("vinput-model.json") != std::string::npos);
}

void TestGetModelInfoInvalidJson() {
  const TempBaseDir temp;
  const fs::path dir = temp.path / "sherpa" / "broken";
  fs::create_directories(dir);
  WriteFile(dir / "vinput-model.json", "{not valid json");

  const ModelManager manager(temp.path.string(), "model.sherpa.broken");
  const auto info = manager.GetModelInfo();
  TEST_ASSERT(!info.has_value());
  TEST_ASSERT(info.error().find("failed to parse") != std::string::npos);
}

void TestValidateRejectsPathEscape() {
  const TempBaseDir temp;
  const fs::path dir = temp.path / "sherpa" / "escape";
  fs::create_directories(dir);
  WriteFile(dir / "tokens.txt", "<blk> 0\n");
  WriteFile(dir / "encoder.onnx", "fake-onnx-weights");
  WriteFile(dir / "vinput-model.json",
            R"json({
  "backend": "sherpa-offline",
  "runtime": "offline",
  "family": "transducer",
  "model": {
    "tokens": "../../outside.txt",
    "transducer": {"encoder": "encoder.onnx"}
  }
})json");

  const ModelManager manager(temp.path.string(), "model.sherpa.escape");
  const auto result = manager.Validate("model.sherpa.escape");
  TEST_ASSERT(!result.has_value());
  TEST_ASSERT(result.error().find("out-of-bounds") != std::string::npos);
}

void TestValidateRejectsMissingAssets() {
  const TempBaseDir temp;

  // tokens.txt declared but absent on disk.
  const fs::path dir = temp.path / "sherpa" / "notokens";
  fs::create_directories(dir);
  WriteFile(dir / "encoder.onnx", "fake-onnx-weights");
  WriteFile(dir / "vinput-model.json",
            R"json({
  "backend": "sherpa-offline",
  "runtime": "offline",
  "family": "transducer",
  "model": {
    "tokens": "tokens.txt",
    "transducer": {"encoder": "encoder.onnx"}
  }
})json");

  const ModelManager manager(temp.path.string(), "model.sherpa.notokens");
  const auto result = manager.Validate("model.sherpa.notokens");
  TEST_ASSERT(!result.has_value());
  TEST_ASSERT(result.error().find("tokens file not found") != std::string::npos);
}

void TestValidateRejectsMissingFamily() {
  const TempBaseDir temp;
  const fs::path dir = temp.path / "sherpa" / "nofamily";
  fs::create_directories(dir);
  WriteFile(dir / "vinput-model.json", R"json({"backend": "sherpa-offline"})json");

  const ModelManager manager(temp.path.string(), "model.sherpa.nofamily");
  const auto result = manager.Validate("model.sherpa.nofamily");
  TEST_ASSERT(!result.has_value());
  TEST_ASSERT(result.error().find("family") != std::string::npos);
}

void TestValidateSuccess() {
  const TempBaseDir temp;
  const std::string id = CreateValidModel(temp.path, "sherpa", "zipformer");
  const ModelManager manager(temp.path.string());
  TEST_ASSERT(manager.Validate(id).has_value());
  TEST_ASSERT(!manager.Validate("model.sherpa.nonexistent").has_value());
  TEST_ASSERT(!manager.Validate("not-a-model-id").has_value());
}

void TestListModelsAndListDetailed() {
  const TempBaseDir temp;
  const std::string id_a = CreateValidModel(temp.path, "sherpa", "alpha");
  const std::string id_b = CreateValidModel(temp.path, "funasr", "beta");

  // A broken model: unparsable metadata must surface as ModelState::Broken.
  const fs::path broken_dir = temp.path / "sherpa" / "broken";
  fs::create_directories(broken_dir);
  WriteFile(broken_dir / "vinput-model.json", "{not valid json");

  const ModelManager manager(temp.path.string());
  const auto ids = manager.ListModels();
  TEST_ASSERT(ids.size() == 3);
  TEST_ASSERT(ids[0] == "model.funasr.beta");
  TEST_ASSERT(ids[1] == "model.sherpa.alpha");
  TEST_ASSERT(ids[2] == "model.sherpa.broken");

  const auto detailed = manager.ListDetailed(id_a);
  TEST_ASSERT(detailed.size() == 3);
  for (const auto& summary : detailed) {
    if (summary.id == id_a) {
      TEST_ASSERT(summary.state == ModelState::Active);
      TEST_ASSERT(summary.model_type == "transducer");
      TEST_ASSERT(summary.language == "zh");
      TEST_ASSERT(summary.supports_hotwords);
      TEST_ASSERT(summary.size_bytes == 42);
    } else if (summary.id == id_b) {
      TEST_ASSERT(summary.state == ModelState::Installed);
    } else {
      TEST_ASSERT(summary.state == ModelState::Broken);
    }
  }
}

void TestRemoveModel() {
  const TempBaseDir temp;
  const std::string id = CreateValidModel(temp.path, "sherpa", "zipformer");
  const fs::path model_dir = temp.path / "sherpa" / "zipformer";

  const ModelManager manager(temp.path.string());
  TEST_ASSERT(fs::exists(model_dir));
  TEST_ASSERT(manager.Remove(id).has_value());
  TEST_ASSERT(!fs::exists(model_dir));

  // Removing again must fail: directory no longer exists.
  const auto again = manager.Remove(id);
  TEST_ASSERT(!again.has_value());
  TEST_ASSERT(again.error().find("does not exist") != std::string::npos);

  // Invalid ids are rejected before touching the filesystem.
  TEST_ASSERT(!manager.Remove("not-a-model-id").has_value());
  TEST_ASSERT(!manager.Remove("model.a..b").has_value());
}

void TestRemoveRejectsBaseDirEscape() {
  const TempBaseDir temp;
  const ModelManager manager(temp.path.string());

  // An id whose canonical path would resolve outside the base dir is rejected
  // by the id grammar itself (path separators are forbidden in model ids).
  const auto result = manager.Remove("model....evil");
  TEST_ASSERT(!result.has_value());
  TEST_ASSERT(result.error().find("invalid model id") != std::string::npos);
}

void TestAndThenChainingAcrossOperations() {
  const TempBaseDir temp;
  const std::string id = CreateValidModel(temp.path, "sherpa", "zipformer");
  const ModelManager manager(temp.path.string(), id);

  // Chain EnsureModels -> GetModelInfo monadically.
  auto chained = manager.EnsureModels().and_then([&manager]() { return manager.GetModelInfo(); });
  TEST_ASSERT(chained.has_value());
  TEST_ASSERT(chained->family == "transducer");

  const ModelManager missing(temp.path.string(), "model.sherpa.absent");
  auto failed = missing.EnsureModels().and_then([&missing]() { return missing.GetModelInfo(); });
  TEST_ASSERT(!failed.has_value());
  TEST_ASSERT(failed.error().find("vinput-model.json") != std::string::npos);
}

} // namespace

int main() {
  TestModelIdPathMapping();
  TestModelDirComposition();
  TestEnsureModelsAndGetModelInfoSuccess();
  TestEnsureModelsMissingMetadata();
  TestGetModelInfoInvalidJson();
  TestValidateRejectsPathEscape();
  TestValidateRejectsMissingAssets();
  TestValidateRejectsMissingFamily();
  TestValidateSuccess();
  TestListModelsAndListDetailed();
  TestRemoveModel();
  TestRemoveRejectsBaseDirEscape();
  TestAndThenChainingAcrossOperations();
  std::printf("All test_model_manager cases passed successfully.\n");
  return 0;
}
