#pragma once

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

struct ModelInfo {
  std::string backend; // e.g. "sherpa-offline", "sherpa-streaming"
  std::string runtime; // "online" or "offline"
  std::string family;  // sherpa-onnx C API family
  std::string model_type; // compatibility alias for family
  std::string title;
  std::string description;
  std::string language = "auto";
  bool supports_hotwords = false;
  uint64_t size_bytes = 0;
  // All file paths (absolute), keyed by role:
  //   "model", "tokens", "encoder", "decoder", "joiner",
  //   "preprocessor", "uncached_decoder", "cached_decoder", "merged_decoder"
  std::map<std::string, std::string> files;
  // Rejected file paths from metadata that resolved outside the model root.
  std::map<std::string, std::string> rejected_files;
  // Flattened scalar parameters for compatibility with old callers.
  std::map<std::string, std::string> params;
  nlohmann::json recognizer_config;
  nlohmann::json model_config;

  // Convenience accessors
  std::string File(const std::string &key) const {
    auto it = files.find(key);
    return it != files.end() ? it->second : std::string{};
  }
  std::string Param(const std::string &key,
                    const std::string &default_val = "") const {
    auto it = params.find(key);
    return it != params.end() ? it->second : default_val;
  }
  bool ParamBool(const std::string &key, bool default_val = false) const {
    auto it = params.find(key);
    if (it == params.end()) return default_val;
    return it->second == "true" || it->second == "1";
  }
  std::string RuntimeLanguageHint() const;
};

enum class ModelState { Installed, Active, Broken };

struct ModelSummary {
  std::string id;
  ModelState state;
  std::string model_type;
  std::string title;
  std::string description;
  std::string language;
  bool supports_hotwords = false;
  uint64_t size_bytes = 0;
};

class ModelManager {
public:
  explicit ModelManager(const std::string &base_dir = "",
                        const std::string &model_id = "");

  bool EnsureModels(std::string *error = nullptr);
  ModelInfo GetModelInfo(std::string *error = nullptr) const;
  std::vector<std::string> ListModels() const;
  std::string GetBaseDir() const;
  std::string GetModelId() const;

  // List all local models with their states
  std::vector<ModelSummary> ListDetailed(const std::string &active_model) const;
  // Validate model directory integrity
  bool Validate(const std::string &model_id, std::string *error) const;
  // Remove a model directory
  bool Remove(const std::string &model_id, std::string *error) const;
  // Normalize base_dir (expand ~)
  static std::filesystem::path NormalizeBaseDir(const std::string &raw_path);
  static std::filesystem::path RelativePathForId(std::string_view model_id);
  static std::string IdFromRelativePath(const std::filesystem::path &relative_path);
  std::filesystem::path ModelDir(std::string_view model_id) const;

private:
  bool IsValidModelDir(const std::string &model_id) const;
  std::string base_dir_;
  std::string model_id_;
};
