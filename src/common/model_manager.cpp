#include "common/model_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "common/path_utils.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// static
fs::path ModelManager::NormalizeBaseDir(const std::string &raw_path) {
  if (raw_path.empty()) {
    return vinput::path::DefaultModelBaseDir();
  }
  return vinput::path::ExpandUserPath(raw_path);
}

ModelManager::ModelManager(const std::string &base_dir,
                           const std::string &model_name) {
  base_dir_ = NormalizeBaseDir(base_dir).string();
  model_name_ = model_name.empty() ? "paraformer-zh" : model_name;
}

bool ModelManager::EnsureModels() {
  auto dir = fs::path(base_dir_) / model_name_;
  auto json_path = dir / "vinput-model.json";

  if (!fs::exists(json_path)) {
    fprintf(stderr,
            "vinput: ASR model configuration not found for '%s' at %s\n"
            "vinput: Missing 'vinput-model.json'. Please ensure you have a "
            "valid model installed.\n",
            model_name_.c_str(), json_path.string().c_str());
    return false;
  }

  auto info = GetModelInfo();
  if (info.model.empty() || info.tokens.empty() || info.model_type.empty()) {
    fprintf(
        stderr,
        "vinput: 'vinput-model.json' for '%s' is missing required fields.\n",
        model_name_.c_str());
    return false;
  }

  if (!fs::exists(info.model)) {
    fprintf(stderr, "vinput: ASR model file not found at %s\n",
            info.model.c_str());
    return false;
  }

  if (!fs::exists(info.tokens)) {
    fprintf(stderr, "vinput: tokens.txt not found at %s\n",
            info.tokens.c_str());
    return false;
  }

  return true;
}

ModelInfo ModelManager::GetModelInfo() const {
  ModelInfo info;
  auto dir = fs::path(base_dir_) / model_name_;
  auto json_path = dir / "vinput-model.json";

  if (!fs::exists(json_path)) {
    return info;
  }

  try {
    std::ifstream file(json_path);
    json j;
    file >> j;

    info.model_type = j.value("model_type", "");
    info.language = j.value("language", "auto");

    if (j.contains("files") && j["files"].is_object()) {
      info.model = (dir / j["files"].value("model", "")).string();
      info.tokens = (dir / j["files"].value("tokens", "")).string();
    }

    if (j.contains("params") && j["params"].is_object()) {
      info.modeling_unit = j["params"].value("modeling_unit", "");
      info.use_itn = j["params"].value("use_itn", false);
    }

  } catch (const std::exception &e) {
    fprintf(stderr, "vinput: failed to parse %s: %s\n",
            json_path.string().c_str(), e.what());
  }

  return info;
}

std::string ModelManager::GetBaseDir() const { return base_dir_; }

std::vector<std::string> ModelManager::ListModels() const {
  std::vector<std::string> models;
  const auto root = fs::path(base_dir_);
  if (!fs::exists(root) || !fs::is_directory(root)) {
    return models;
  }

  for (const auto &entry : fs::directory_iterator(root)) {
    if (!entry.is_directory()) {
      continue;
    }

    const auto model_name = entry.path().filename().string();
    if (IsValidModelDir(model_name)) {
      models.push_back(model_name);
    }
  }

  std::sort(models.begin(), models.end());
  return models;
}

std::string ModelManager::GetModelName() const { return model_name_; }

bool ModelManager::IsValidModelDir(const std::string &model_name) const {
  const auto dir = fs::path(base_dir_) / model_name;
  const auto json_path = dir / "vinput-model.json";
  return fs::exists(json_path) && fs::is_regular_file(json_path);
}

std::vector<ModelSummary>
ModelManager::ListDetailed(const std::string &active_model) const {
  std::vector<ModelSummary> summaries;
  const auto root = fs::path(base_dir_);
  if (!fs::exists(root) || !fs::is_directory(root)) {
    return summaries;
  }

  for (const auto &entry : fs::directory_iterator(root)) {
    if (!entry.is_directory()) {
      continue;
    }

    const auto name = entry.path().filename().string();
    ModelSummary s;
    s.name = name;

    if (!IsValidModelDir(name)) {
      s.state = ModelState::Broken;
      summaries.push_back(std::move(s));
      continue;
    }

    // Parse model type and language from vinput-model.json
    const auto json_path = entry.path() / "vinput-model.json";
    try {
      std::ifstream file(json_path);
      json j;
      file >> j;
      s.model_type = j.value("model_type", "");
      s.language = j.value("language", "auto");
    } catch (...) {
      s.state = ModelState::Broken;
      summaries.push_back(std::move(s));
      continue;
    }

    s.state = (name == active_model) ? ModelState::Active : ModelState::Installed;
    summaries.push_back(std::move(s));
  }

  std::sort(summaries.begin(), summaries.end(),
            [](const ModelSummary &a, const ModelSummary &b) {
              return a.name < b.name;
            });
  return summaries;
}

bool ModelManager::Validate(const std::string &model_name,
                            std::string *error) const {
  const auto dir = fs::path(base_dir_) / model_name;

  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    if (error) *error = "model directory does not exist: " + dir.string();
    return false;
  }

  const auto json_path = dir / "vinput-model.json";
  if (!fs::exists(json_path) || !fs::is_regular_file(json_path)) {
    if (error) *error = "vinput-model.json not found in " + dir.string();
    return false;
  }

  ModelInfo info;
  try {
    std::ifstream file(json_path);
    json j;
    file >> j;

    info.model_type = j.value("model_type", "");
    info.language = j.value("language", "auto");

    if (j.contains("files") && j["files"].is_object()) {
      info.model = (dir / j["files"].value("model", "")).string();
      info.tokens = (dir / j["files"].value("tokens", "")).string();
    }
  } catch (const std::exception &e) {
    if (error)
      *error = std::string("failed to parse vinput-model.json: ") + e.what();
    return false;
  }

  if (info.model_type.empty()) {
    if (error) *error = "vinput-model.json missing required field: model_type";
    return false;
  }

  if (info.model.empty()) {
    if (error) *error = "vinput-model.json missing required field: files.model";
    return false;
  }

  if (!fs::exists(info.model)) {
    if (error) *error = "model file not found: " + info.model;
    return false;
  }

  if (info.tokens.empty()) {
    if (error)
      *error = "vinput-model.json missing required field: files.tokens";
    return false;
  }

  if (!fs::exists(info.tokens)) {
    if (error) *error = "tokens file not found: " + info.tokens;
    return false;
  }

  return true;
}

bool ModelManager::Remove(const std::string &model_name,
                          std::string *error) const {
  const auto dir = fs::path(base_dir_) / model_name;

  if (!fs::exists(dir)) {
    if (error) *error = "model directory does not exist: " + dir.string();
    return false;
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
  if (ec) {
    if (error)
      *error = "failed to remove model directory: " + ec.message();
    return false;
  }

  return true;
}
