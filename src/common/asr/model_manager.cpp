#include "common/asr/model_manager.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <glaze/core/common.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/json/json_access.h"
#include "common/utils/path_utils.h"
#include "common/utils/string_utils.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers: parse vinput-model.json into ModelInfo
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> SplitModelId(std::string_view model_id) {
  if (model_id.empty() || model_id == "." || model_id == ".." ||
      model_id.find('/') != std::string_view::npos ||
      model_id.find('\\') != std::string_view::npos) {
    return {};
  }

  const auto tokens = vinput::str::Split(model_id, '.');
  if (tokens.size() < 3 || tokens.front() != "model") {
    return {};
  }

  std::vector<std::string> raw_segments;
  raw_segments.reserve(tokens.size());
  for (const auto& token : tokens) {
    if (token.empty() || token == "." || token == "..") {
      return {};
    }
    raw_segments.emplace_back(token);
  }

  const std::string& source = raw_segments[1];
  std::string name;
  for (std::size_t i = 2; i < raw_segments.size(); ++i) {
    if (!name.empty()) {
      name += '.';
    }
    name += raw_segments[i];
  }
  if (source.empty() || name.empty()) {
    return {};
  }

  return {"model", source, name};
}

// Walk only the install layout produced by ModelRepository::InstallModel:
//   <base>/<engine>/<name>/vinput-model.json
// corresponding to id model.<engine>.<name>. Never recurse into model file trees.
template <typename Fn> void ForEachInstalledModelDir(const std::string& base_dir, Fn fn) {
  const auto root = fs::path(base_dir);
  std::error_code ec;
  if (!fs::exists(root, ec) || ec || !fs::is_directory(root, ec) || ec) {
    return;
  }

  for (const auto& engine_entry : fs::directory_iterator(root, ec)) {
    if (ec) {
      return;
    }
    if (!engine_entry.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    const auto& engine_dir = engine_entry.path();
    const std::string engine_name = engine_dir.filename().string();
    if (engine_name.empty() || engine_name == "." || engine_name == ".." ||
        engine_name.front() == '.') {
      continue;
    }

    for (const auto& model_entry : fs::directory_iterator(engine_dir, ec)) {
      if (ec) {
        ec.clear();
        break;
      }
      if (!model_entry.is_directory(ec) || ec) {
        ec.clear();
        continue;
      }
      const auto& model_dir = model_entry.path();
      const std::string model_name = model_dir.filename().string();
      if (model_name.empty() || model_name == "." || model_name == ".." ||
          model_name.front() == '.') {
        continue;
      }

      const fs::path relative_path = fs::path(engine_name) / model_name;
      const std::string model_id = ModelManager::IdFromRelativePath(relative_path);
      if (model_id.empty()) {
        continue;
      }

      const fs::path json_path = model_dir / "vinput-model.json";
      if (!fs::is_regular_file(json_path, ec) || ec) {
        ec.clear();
        continue;
      }

      fn(model_id, model_dir, json_path);
    }
  }
}

bool ResolveModelMetadataPath(const fs::path& model_dir, const fs::path& model_root,
                              const std::string& raw_path, fs::path* resolved, std::string* error) {
  std::error_code ec;
  fs::path candidate = fs::weakly_canonical(model_dir / fs::path(raw_path), ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to resolve path '" + raw_path + "': " + ec.message();
    }
    return false;
  }
  if (!vinput::path::IsPathWithinRoot(candidate, model_root)) {
    if (error != nullptr) {
      *error = "path escapes model directory: " + raw_path;
    }
    return false;
  }
  if (resolved != nullptr) {
    *resolved = std::move(candidate);
  }
  return true;
}

// Integral JSON numbers are rendered without a fractional part so that the
// string parameter map keeps the exact spelling of the model metadata.
std::string FormatNumber(double value) {
  if (value == static_cast<double>(static_cast<std::int64_t>(value))) {
    return std::to_string(static_cast<std::int64_t>(value));
  }
  return std::to_string(value);
}

void AddScalarParam(std::map<std::string, std::string>* params, std::string_view key,
                    const glz::json_t& value) {
  if (params == nullptr || key.empty()) {
    return;
  }
  if (const auto* text = value.get_if<std::string>()) {
    (*params)[std::string(key)] = *text;
  } else if (const auto* flag = value.get_if<bool>()) {
    (*params)[std::string(key)] = *flag ? "true" : "false";
  } else if (const auto* number = value.get_if<double>()) {
    (*params)[std::string(key)] = FormatNumber(*number);
  }
}

void AddScalarParam(std::map<std::string, std::string>* params, std::string_view key,
                    const glz::json_t& object, std::string_view field, double fallback) {
  const auto* node = vinput::json::Find(object, field);
  if (node == nullptr) {
    AddScalarParam(params, key, glz::json_t(fallback));
    return;
  }
  AddScalarParam(params, key, *node);
}

void AddScalarParam(std::map<std::string, std::string>* params, std::string_view key,
                    const glz::json_t& object, std::string_view field, std::string_view fallback) {
  const auto* node = vinput::json::Find(object, field);
  if (node == nullptr) {
    AddScalarParam(params, key, glz::json_t(std::string(fallback)));
    return;
  }
  AddScalarParam(params, key, *node);
}

void AddScalarParam(std::map<std::string, std::string>* params, std::string_view key,
                    const glz::json_t& object, std::string_view field, int fallback) {
  AddScalarParam(params, key, object, field, static_cast<double>(fallback));
}

void ResolveModelFileField(const fs::path& dir, const fs::path& model_root, const glz::json_t& obj,
                           std::string_view field, std::string_view file_key, ModelInfo* info) {
  const auto* node = vinput::json::Find(obj, field);
  if (info == nullptr || node == nullptr || !node->is_string()) {
    return;
  }
  const std::string raw_path = node->get<std::string>();
  if (raw_path.empty()) {
    return;
  }
  fs::path resolved_path;
  std::string path_error;
  if (!ResolveModelMetadataPath(dir, model_root, raw_path, &resolved_path, &path_error)) {
    info->rejected_files[std::string(file_key)] = raw_path;
    return;
  }
  info->files[std::string(file_key)] = resolved_path.string();
}

bool FamilyUsesTokenizerAsset(std::string_view family) {
  return family == "funasr_nano" || family == "qwen3_asr";
}

ModelInfo ParseModelJson(const fs::path& dir, const fs::path& json_path, std::string* error) {
  ModelInfo info;
  std::ifstream file(json_path);
  const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

  glz::json_t j;
  if (const auto ec = glz::read_json(j, content); bool(ec)) {
    if (error != nullptr) {
      *error = "failed to parse '" + json_path.string() + "': " + glz::format_error(ec, content);
    }
    return info;
  }

  std::error_code root_ec;
  const fs::path model_root = fs::weakly_canonical(dir, root_ec);
  if (root_ec) {
    if (error != nullptr) {
      *error = "failed to resolve model root '" + dir.string() + "': " + root_ec.message();
    }
    return info;
  }

  info.backend = vinput::json::String(j, "backend");
  info.runtime = vinput::json::String(j, "runtime");
  info.family = vinput::json::String(j, "family");
  info.model_type = info.family;
  info.language = vinput::json::String(j, "language", "auto");
  info.supports_hotwords = vinput::json::Bool(j, "supports_hotwords");
  info.size_bytes = static_cast<uint64_t>(vinput::json::Int64(j, "size_bytes", 0));
  if (const auto* recognizer = vinput::json::Find(j, "recognizer");
      recognizer != nullptr && recognizer->is_object()) {
    info.recognizer_config = *recognizer;
  }
  if (const auto* model = vinput::json::Find(j, "model"); model != nullptr && model->is_object()) {
    info.model_config = *model;
  }

  ResolveModelFileField(dir, model_root, info.model_config, "tokens", "tokens", &info);
  ResolveModelFileField(dir, model_root, info.model_config, "bpe_vocab", "bpe_vocab", &info);
  ResolveModelFileField(dir, model_root, info.model_config, "bpe_model", "bpe_model", &info);
  ResolveModelFileField(dir, model_root, info.model_config, "telespeech_ctc", "telespeech_ctc",
                        &info);

  if (const auto* lm_config = vinput::json::Find(info.recognizer_config, "lm_config");
      lm_config != nullptr && lm_config->is_object()) {
    ResolveModelFileField(dir, model_root, *lm_config, "model", "lm", &info);
    AddScalarParam(&info.params, "lm_scale", *lm_config, "scale", 0.0);
  }
  ResolveModelFileField(dir, model_root, info.recognizer_config, "hotwords_file", "hotwords_file",
                        &info);
  ResolveModelFileField(dir, model_root, info.recognizer_config, "rule_fsts", "rule_fsts", &info);
  ResolveModelFileField(dir, model_root, info.recognizer_config, "rule_fars", "rule_fars", &info);
  if (const auto* ctc_config = vinput::json::Find(info.recognizer_config, "ctc_fst_decoder_config");
      ctc_config != nullptr && ctc_config->is_object()) {
    ResolveModelFileField(dir, model_root, *ctc_config, "graph", "graph", &info);
    AddScalarParam(&info.params, "ctc_fst_max_active", *ctc_config, "max_active", 0);
  }
  if (const auto* hr_config = vinput::json::Find(info.recognizer_config, "hr");
      hr_config != nullptr && hr_config->is_object()) {
    ResolveModelFileField(dir, model_root, *hr_config, "lexicon", "hr_lexicon", &info);
    ResolveModelFileField(dir, model_root, *hr_config, "rule_fsts", "hr_rule_fsts", &info);
  }

  AddScalarParam(&info.params, "decoding_method", info.recognizer_config, "decoding_method",
                 std::string{});
  AddScalarParam(&info.params, "max_active_paths", info.recognizer_config, "max_active_paths", 0);
  AddScalarParam(&info.params, "enable_endpoint", info.recognizer_config, "enable_endpoint", 0);
  AddScalarParam(&info.params, "rule1_min_trailing_silence", info.recognizer_config,
                 "rule1_min_trailing_silence", 0.0);
  AddScalarParam(&info.params, "rule2_min_trailing_silence", info.recognizer_config,
                 "rule2_min_trailing_silence", 0.0);
  AddScalarParam(&info.params, "rule3_min_utterance_length", info.recognizer_config,
                 "rule3_min_utterance_length", 0.0);
  AddScalarParam(&info.params, "hotwords_score", info.recognizer_config, "hotwords_score", 0.0);
  AddScalarParam(&info.params, "blank_penalty", info.recognizer_config, "blank_penalty", 0.0);

  AddScalarParam(&info.params, "num_threads", info.model_config, "num_threads", 0);
  AddScalarParam(&info.params, "debug", info.model_config, "debug", 0);
  AddScalarParam(&info.params, "provider", info.model_config, "provider", std::string{});
  AddScalarParam(&info.params, "model_type", info.model_config, "model_type", std::string{});
  AddScalarParam(&info.params, "modeling_unit", info.model_config, "modeling_unit", std::string{});

  if (const auto* family_config = vinput::json::Find(info.model_config, info.family);
      family_config != nullptr && family_config->is_object()) {
    for (const auto& [key, value] : family_config->get_object()) {
      if (value.is_string()) {
        ResolveModelFileField(dir, model_root, *family_config, key, key, &info);
      }
      AddScalarParam(&info.params, key, value);
    }
  }

  return info;
}

// Check that the tokens file exists (required for all model types)
bool HasTokens(const ModelInfo& info) {
  return !info.File("tokens").empty() && fs::exists(info.File("tokens"));
}

bool HasTokenizer(const ModelInfo& info) {
  return !info.File("tokenizer").empty() && fs::exists(info.File("tokenizer"));
}

bool HasRequiredTextAsset(const ModelInfo& info) {
  return FamilyUsesTokenizerAsset(info.family) ? HasTokenizer(info) : HasTokens(info);
}

std::string RequiredTextAssetField(const ModelInfo& info) {
  return FamilyUsesTokenizerAsset(info.family) ? "model.tokenizer" : "model.tokens";
}

std::string RequiredTextAssetPathKey(const ModelInfo& info) {
  return FamilyUsesTokenizerAsset(info.family) ? "tokenizer" : "tokens";
}

// Check that at least one model/encoder file exists
bool HasModelFiles(const ModelInfo& info) {
  return std::ranges::any_of(info.files, [](const auto& pair) {
    const auto& [key, path] = pair;
    if (key == "tokens" || key == "tokenizer" || key == "bpe_vocab" || key == "bpe_model" ||
        key == "hotwords_file" || key == "lm" || key == "rule_fsts" || key == "rule_fars" ||
        key == "graph" || key == "hr_lexicon" || key == "hr_rule_fsts") {
      return false;
    }
    return !path.empty() && fs::exists(path);
  });
}

} // namespace

std::string ModelInfo::RuntimeLanguageHint() const {
  auto looks_like_runtime_hint = [](std::string_view value) {
    if (value.empty()) {
      return false;
    }
    for (const char c : value) {
      const bool ok = (c >= 'a' && c <= 'z') || c == '-' || c == '_';
      if (!ok) {
        return false;
      }
    }
    return value != "multilingual" && value.find('_') == std::string_view::npos;
  };

  if (model_config.contains(family) && model_config[family].is_object()) {
    const auto& family_config = model_config[family];
    if (family_config.contains("language") && family_config["language"].is_string()) {
      return family_config["language"].get<std::string>();
    }
    if (family_config.contains("src_lang") && family_config["src_lang"].is_string()) {
      return family_config["src_lang"].get<std::string>();
    }
  }

  if (looks_like_runtime_hint(language)) {
    return language;
  }

  return {};
}

// ---------------------------------------------------------------------------
// ModelManager
// ---------------------------------------------------------------------------

// static
fs::path ModelManager::NormalizeBaseDir(const std::string& raw_path) {
  if (raw_path.empty()) {
    return vinput::path::DefaultModelBaseDir();
  }
  return vinput::path::ExpandUserPath(raw_path);
}

fs::path ModelManager::RelativePathForId(std::string_view model_id) {
  const auto segments = SplitModelId(model_id);
  if (segments.size() != 3 || segments.front() != "model") {
    return {};
  }

  fs::path relative_path;
  for (std::size_t i = 1; i < segments.size(); ++i) {
    relative_path /= segments[i];
  }
  return relative_path;
}

std::string ModelManager::IdFromRelativePath(const std::filesystem::path& relative_path) {
  std::vector<std::string> relative_segments;
  for (const auto& component : relative_path) {
    const std::string part = component.string();
    if (part.empty() || part == "." || part == "..") {
      return {};
    }
    relative_segments.push_back(part);
  }
  if (relative_segments.size() != 2) {
    return {};
  }

  std::vector<std::string> segments = {"model"};
  segments.insert(segments.end(), relative_segments.begin(), relative_segments.end());
  std::string model_id;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i > 0) {
      model_id += '.';
    }
    model_id += segments[i];
  }
  return model_id;
}

fs::path ModelManager::ModelDir(std::string_view model_id) const {
  const fs::path relative_path = RelativePathForId(model_id);
  if (relative_path.empty()) {
    return {};
  }
  return fs::path(base_dir_) / relative_path;
}

ModelManager::ModelManager(const std::string& base_dir, std::string model_id)
    : base_dir_(NormalizeBaseDir(base_dir).string()), model_id_(std::move(model_id)) {}

bool ModelManager::EnsureModels(std::string* error) {
  auto dir = ModelDir(model_id_);
  auto json_path = dir / "vinput-model.json";

  if (!fs::exists(json_path)) {
    if (error != nullptr) {
      *error = "missing 'vinput-model.json' in " + dir.string();
    }
    return false;
  }

  std::string parse_error;
  auto info = GetModelInfo(&parse_error);
  if (info.family.empty()) {
    if (error != nullptr) {
      if (!parse_error.empty()) {
        *error = std::move(parse_error);
      } else {
        *error = "'vinput-model.json' is missing family for model '" + model_id_ + "'";
      }
    }
    return false;
  }

  if (!info.rejected_files.empty()) {
    const auto& [key, raw_path] = *info.rejected_files.begin();
    if (error != nullptr) {
      *error = "'vinput-model.json' contains invalid path for '" + key + "': " + raw_path;
    }
    return false;
  }

  if (!HasRequiredTextAsset(info)) {
    if (error != nullptr) {
      *error = RequiredTextAssetPathKey(info) + " file not found for model '" + model_id_ + "'";
    }
    return false;
  }

  if (!HasModelFiles(info)) {
    if (error != nullptr) {
      *error = "no model files found for model '" + model_id_ + "'";
    }
    return false;
  }

  return true;
}

ModelInfo ModelManager::GetModelInfo(std::string* error) const {
  auto dir = ModelDir(model_id_);
  auto json_path = dir / "vinput-model.json";

  if (!fs::exists(json_path)) {
    return {};
  }

  return ParseModelJson(dir, json_path, error);
}

std::string ModelManager::GetBaseDir() const {
  return base_dir_;
}

std::vector<std::string> ModelManager::ListModels() const {
  std::vector<std::string> models;
  // Install layout is fixed: models/<engine>/<name>/vinput-model.json
  // (id: model.<engine>.<name>). Only walk that shallow tree.
  ForEachInstalledModelDir(base_dir_, [&](const std::string& model_id, const fs::path&,
                                          const fs::path&) { models.push_back(model_id); });

  std::ranges::sort(models);
  models.erase(std::ranges::unique(models).begin(), models.end());
  return models;
}

std::string ModelManager::GetModelId() const {
  return model_id_;
}

std::vector<ModelSummary> ModelManager::ListDetailed(const std::string& active_model) const {
  std::vector<ModelSummary> summaries;
  // Same shallow install layout as ListModels / ModelRepository::InstallModel.
  ForEachInstalledModelDir(
      base_dir_, [&](const std::string& model_id, const fs::path&, const fs::path& json_path) {
        ModelSummary s;
        s.id = model_id;
        std::ifstream file(json_path);
        const std::string content{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
        glz::json_t j;
        const bool typed_ok = !bool(glz::read_json(j, content)) && j.is_object() && [&j] {
          const auto* family = vinput::json::Find(j, "family");
          const auto* language = vinput::json::Find(j, "language");
          const auto* hotwords = vinput::json::Find(j, "supports_hotwords");
          const auto* size = vinput::json::Find(j, "size_bytes");
          return (family == nullptr || family->is_string()) &&
                 (language == nullptr || language->is_string()) &&
                 (hotwords == nullptr || hotwords->is_boolean()) &&
                 (size == nullptr || size->is_number());
        }();
        if (!typed_ok) {
          s.state = ModelState::Broken;
          summaries.push_back(std::move(s));
          return;
        }
        s.model_type = vinput::json::String(j, "family");
        s.language = vinput::json::String(j, "language", "auto");
        s.supports_hotwords = vinput::json::Bool(j, "supports_hotwords");
        s.size_bytes = static_cast<uint64_t>(vinput::json::Int64(j, "size_bytes", 0));

        s.state = (model_id == active_model) ? ModelState::Active : ModelState::Installed;
        summaries.push_back(std::move(s));
      });

  std::ranges::sort(summaries,
                    [](const ModelSummary& a, const ModelSummary& b) { return a.id < b.id; });
  return summaries;
}

bool ModelManager::Validate(const std::string& model_id, std::string* error) const {
  const auto dir = ModelDir(model_id);
  if (dir.empty()) {
    if (error != nullptr) {
      *error = "invalid model id: " + model_id;
    }
    return false;
  }

  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    if (error != nullptr) {
      *error = "model directory does not exist: " + dir.string();
    }
    return false;
  }

  const auto json_path = dir / "vinput-model.json";
  if (!fs::exists(json_path) || !fs::is_regular_file(json_path)) {
    if (error != nullptr) {
      *error = "vinput-model.json not found in " + dir.string();
    }
    return false;
  }

  std::string parse_error;
  auto info = ParseModelJson(dir, json_path, &parse_error);

  if (info.family.empty()) {
    if (error != nullptr) {
      if (!parse_error.empty()) {
        *error = std::move(parse_error);
      } else {
        *error = "vinput-model.json missing required field: family";
      }
    }
    return false;
  }

  if (!info.rejected_files.empty()) {
    const auto& [key, raw_path] = *info.rejected_files.begin();
    if (error != nullptr) {
      *error = "vinput-model.json contains out-of-bounds file path for '" + key + "': " + raw_path;
    }
    return false;
  }

  if (!HasRequiredTextAsset(info)) {
    const auto path_key = RequiredTextAssetPathKey(info);
    auto asset_path = info.File(path_key);
    if (asset_path.empty()) {
      if (error != nullptr) {
        *error = "vinput-model.json missing required field: " + RequiredTextAssetField(info);
      }
    } else {
      if (error != nullptr) {
        *error = path_key + " file not found: " + asset_path;
      }
    }
    return false;
  }

  if (!HasModelFiles(info)) {
    if (error != nullptr) {
      *error = "no model/encoder files found in model directory";
    }
    return false;
  }

  return true;
}

bool ModelManager::Remove(const std::string& model_id, std::string* error) const {
  const fs::path requested_dir = ModelDir(model_id);
  if (requested_dir.empty()) {
    if (error != nullptr) {
      *error = "invalid model id: " + model_id;
    }
    return false;
  }

  std::error_code ec;
  const auto dir = fs::weakly_canonical(requested_dir, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to resolve model directory: " + ec.message();
    }
    return false;
  }
  const auto base = fs::weakly_canonical(fs::path(base_dir_), ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to resolve model base directory: " + ec.message();
    }
    return false;
  }

  if (!vinput::path::IsPathWithinRoot(dir, base)) {
    if (error != nullptr) {
      *error = "invalid model id: " + model_id;
    }
    return false;
  }

  if (!fs::exists(dir)) {
    if (error != nullptr) {
      *error = "model directory does not exist: " + dir.string();
    }
    return false;
  }

  fs::remove_all(dir, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to remove model directory: " + ec.message();
    }
    return false;
  }

  return true;
}
