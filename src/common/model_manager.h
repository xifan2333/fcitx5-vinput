#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct ModelInfo {
  std::string model_type; // e.g. "paraformer", "sense_voice", "whisper"
  // All file paths (absolute), keyed by role:
  //   "model", "tokens", "encoder", "decoder", "joiner",
  //   "preprocessor", "uncached_decoder", "cached_decoder", "merged_decoder"
  std::map<std::string, std::string> files;
  // Model-specific parameters from vinput-model.json "params"
  std::map<std::string, std::string> params;

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
};

enum class ModelState { Installed, Active, Broken };

struct ModelSummary {
  std::string name;
  ModelState state;
  std::string model_type;
  std::string language;
  bool supports_hotwords = false;
};

class ModelManager {
public:
  explicit ModelManager(const std::string &base_dir = "",
                        const std::string &model_name = "paraformer-zh");

  bool EnsureModels();
  ModelInfo GetModelInfo() const;
  std::vector<std::string> ListModels() const;
  std::string GetBaseDir() const;
  std::string GetModelName() const;

  // List all local models with their states
  std::vector<ModelSummary> ListDetailed(const std::string &active_model) const;
  // Validate model directory integrity
  bool Validate(const std::string &model_name, std::string *error) const;
  // Remove a model directory
  bool Remove(const std::string &model_name, std::string *error) const;
  // Normalize base_dir (expand ~)
  static std::filesystem::path NormalizeBaseDir(const std::string &raw_path);

private:
  bool IsValidModelDir(const std::string &model_name) const;
  std::string base_dir_;
  std::string model_name_;
};
