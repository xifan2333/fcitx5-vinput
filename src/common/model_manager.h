#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ModelInfo {
  std::string model_type; // e.g. "paraformer", "sense_voice", "whisper"
  std::string language;
  std::string model;  // Abs path to model.onnx or model.int8.onnx
  std::string tokens; // Abs path to tokens.txt
  std::string modeling_unit;
  bool use_itn = false;
};

enum class ModelState { Installed, Active, Broken };

struct ModelSummary {
  std::string name;
  ModelState state;
  std::string model_type;
  std::string language;
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
