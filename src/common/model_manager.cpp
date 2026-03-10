#include "common/model_manager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "common/path_utils.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers: parse vinput-model.json into ModelInfo
// ---------------------------------------------------------------------------

namespace {

ModelInfo ParseModelJson(const fs::path &dir, const fs::path &json_path) {
  ModelInfo info;
  try {
    std::ifstream file(json_path);
    json j;
    file >> j;

    info.model_type = j.value("model_type", "");

    if (j.contains("files") && j["files"].is_object()) {
      for (const auto &[key, val] : j["files"].items()) {
        if (val.is_string() && !val.get<std::string>().empty()) {
          info.files[key] = (dir / val.get<std::string>()).string();
        }
      }
    }

    // Read all params as string key-value pairs
    if (j.contains("params") && j["params"].is_object()) {
      for (const auto &[key, val] : j["params"].items()) {
        if (val.is_string()) {
          info.params[key] = val.get<std::string>();
        } else if (val.is_boolean()) {
          info.params[key] = val.get<bool>() ? "true" : "false";
        } else if (val.is_number_integer()) {
          info.params[key] = std::to_string(val.get<int64_t>());
        } else if (val.is_number_float()) {
          info.params[key] = std::to_string(val.get<double>());
        }
      }
    }

  } catch (const std::exception &e) {
    fprintf(stderr, "vinput: failed to parse %s: %s\n",
            json_path.string().c_str(), e.what());
  }

  return info;
}

// Check that the tokens file exists (required for all model types)
bool HasTokens(const ModelInfo &info) {
  return !info.File("tokens").empty() && fs::exists(info.File("tokens"));
}

// Check that at least one model/encoder file exists
bool HasModelFiles(const ModelInfo &info) {
  for (const auto &[key, path] : info.files) {
    if (key == "tokens") continue;
    if (!path.empty() && fs::exists(path)) return true;
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// ModelManager
// ---------------------------------------------------------------------------

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
  if (info.model_type.empty()) {
    fprintf(
        stderr,
        "vinput: 'vinput-model.json' for '%s' is missing model_type.\n",
        model_name_.c_str());
    return false;
  }

  if (!HasTokens(info)) {
    fprintf(stderr, "vinput: tokens file not found for model '%s'\n",
            model_name_.c_str());
    return false;
  }

  if (!HasModelFiles(info)) {
    fprintf(stderr, "vinput: no model files found for model '%s'\n",
            model_name_.c_str());
    return false;
  }

  return true;
}

ModelInfo ModelManager::GetModelInfo() const {
  auto dir = fs::path(base_dir_) / model_name_;
  auto json_path = dir / "vinput-model.json";

  if (!fs::exists(json_path)) {
    return {};
  }

  return ParseModelJson(dir, json_path);
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

  auto info = ParseModelJson(dir, json_path);

  if (info.model_type.empty()) {
    if (error) *error = "vinput-model.json missing required field: model_type";
    return false;
  }

  if (!HasTokens(info)) {
    auto tokens_path = info.File("tokens");
    if (tokens_path.empty()) {
      if (error) *error = "vinput-model.json missing required field: files.tokens";
    } else {
      if (error) *error = "tokens file not found: " + tokens_path;
    }
    return false;
  }

  if (!HasModelFiles(info)) {
    if (error) *error = "no model/encoder files found in model directory";
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
