#include "common/asr/model_manager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "common/utils/path_utils.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers: parse vinput-model.json into ModelInfo
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> SplitModelId(std::string_view model_id) {
  std::vector<std::string> segments;
  std::size_t start = 0;
  while (start <= model_id.size()) {
    const std::size_t dot = model_id.find('.', start);
    const std::size_t end =
        dot == std::string_view::npos ? model_id.size() : dot;
    if (end == start) {
      return {};
    }
    std::string segment(model_id.substr(start, end - start));
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.find('/') != std::string::npos ||
        segment.find('\\') != std::string::npos) {
      return {};
    }
    segments.push_back(std::move(segment));
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  return segments;
}

bool HasManagedModelPathDepth(const std::vector<std::string> &segments) {
  std::size_t start_index = 0;
  if (!segments.empty() && segments.front() == "model") {
    start_index = 1;
  }
  return segments.size() >= start_index + 2;
}

bool IsPathWithinRoot(const fs::path &path, const fs::path &root) {
  auto path_it = path.begin();
  auto root_it = root.begin();
  for (; root_it != root.end(); ++root_it, ++path_it) {
    if (path_it == path.end() || *path_it != *root_it) {
      return false;
    }
  }
  return true;
}

bool ResolveModelMetadataPath(const fs::path &model_dir,
                              const fs::path &model_root,
                              const std::string &raw_path, fs::path *resolved,
                              std::string *error) {
  std::error_code ec;
  fs::path candidate = fs::weakly_canonical(model_dir / fs::path(raw_path), ec);
  if (ec) {
    if (error) {
      *error = "failed to resolve path '" + raw_path + "': " + ec.message();
    }
    return false;
  }
  if (!IsPathWithinRoot(candidate, model_root)) {
    if (error) {
      *error = "path escapes model directory: " + raw_path;
    }
    return false;
  }
  if (resolved) {
    *resolved = std::move(candidate);
  }
  return true;
}

void AddScalarParam(std::map<std::string, std::string> *params,
                    std::string_view key, const json &value) {
  if (!params || key.empty()) {
    return;
  }
  if (value.is_string()) {
    (*params)[std::string(key)] = value.get<std::string>();
  } else if (value.is_boolean()) {
    (*params)[std::string(key)] = value.get<bool>() ? "true" : "false";
  } else if (value.is_number_integer()) {
    (*params)[std::string(key)] = std::to_string(value.get<int64_t>());
  } else if (value.is_number_float()) {
    (*params)[std::string(key)] = std::to_string(value.get<double>());
  }
}

void ResolveModelFileField(const fs::path &dir, const fs::path &model_root,
                           const json &obj, std::string_view field,
                           std::string_view file_key, ModelInfo *info) {
  if (!info || !obj.is_object() || !obj.contains(field) || !obj[field].is_string()) {
    return;
  }
  const std::string raw_path = obj[field].get<std::string>();
  if (raw_path.empty()) {
    return;
  }
  fs::path resolved_path;
  std::string path_error;
  if (!ResolveModelMetadataPath(dir, model_root, raw_path, &resolved_path,
                                &path_error)) {
    info->rejected_files[std::string(file_key)] = raw_path;
    return;
  }
  info->files[std::string(file_key)] = resolved_path.string();
}

bool FamilyUsesTokenizerAsset(std::string_view family) {
  return family == "funasr_nano" || family == "qwen3_asr";
}

ModelInfo ParseModelJson(const fs::path &dir, const fs::path &json_path,
                         std::string *error) {
  ModelInfo info;
  try {
    std::ifstream file(json_path);
    json j;
    file >> j;
    std::error_code root_ec;
    const fs::path model_root = fs::weakly_canonical(dir, root_ec);
    if (root_ec) {
      if (error) {
        *error = "failed to resolve model root '" + dir.string() +
                 "': " + root_ec.message();
      }
      return info;
    }

    info.backend = j.value("backend", "");
    info.runtime = j.value("runtime", "");
    info.family = j.value("family", "");
    info.model_type = info.family;
    info.language = j.value("language", "auto");
    info.supports_hotwords = j.value("supports_hotwords", false);
    info.size_bytes = j.value("size_bytes", uint64_t{0});
    info.recognizer_config =
        j.contains("recognizer") && j["recognizer"].is_object()
            ? j["recognizer"]
            : json::object();
    info.model_config =
        j.contains("model") && j["model"].is_object() ? j["model"]
                                                       : json::object();

    ResolveModelFileField(dir, model_root, info.model_config, "tokens", "tokens",
                          &info);
    ResolveModelFileField(dir, model_root, info.model_config, "bpe_vocab",
                          "bpe_vocab", &info);
    ResolveModelFileField(dir, model_root, info.model_config, "telespeech_ctc",
                          "telespeech_ctc", &info);

    if (info.recognizer_config.contains("lm_config") &&
        info.recognizer_config["lm_config"].is_object()) {
      ResolveModelFileField(dir, model_root, info.recognizer_config["lm_config"],
                            "model", "lm", &info);
      AddScalarParam(&info.params, "lm_scale",
                     info.recognizer_config["lm_config"].value("scale", 0.0));
    }
    ResolveModelFileField(dir, model_root, info.recognizer_config,
                          "hotwords_file", "hotwords_file", &info);
    ResolveModelFileField(dir, model_root, info.recognizer_config, "rule_fsts",
                          "rule_fsts", &info);
    ResolveModelFileField(dir, model_root, info.recognizer_config, "rule_fars",
                          "rule_fars", &info);
    if (info.recognizer_config.contains("ctc_fst_decoder_config") &&
        info.recognizer_config["ctc_fst_decoder_config"].is_object()) {
      ResolveModelFileField(dir, model_root,
                            info.recognizer_config["ctc_fst_decoder_config"],
                            "graph", "graph", &info);
      AddScalarParam(&info.params, "ctc_fst_max_active",
                     info.recognizer_config["ctc_fst_decoder_config"].value(
                         "max_active", 0));
    }
    if (info.recognizer_config.contains("hr") &&
        info.recognizer_config["hr"].is_object()) {
      ResolveModelFileField(dir, model_root, info.recognizer_config["hr"],
                            "lexicon", "hr_lexicon", &info);
      ResolveModelFileField(dir, model_root, info.recognizer_config["hr"],
                            "rule_fsts", "hr_rule_fsts", &info);
    }

    AddScalarParam(&info.params, "decoding_method",
                   info.recognizer_config.value("decoding_method", ""));
    AddScalarParam(&info.params, "max_active_paths",
                   info.recognizer_config.value("max_active_paths", 0));
    AddScalarParam(&info.params, "enable_endpoint",
                   info.recognizer_config.value("enable_endpoint", 0));
    AddScalarParam(&info.params, "rule1_min_trailing_silence",
                   info.recognizer_config.value("rule1_min_trailing_silence",
                                                0.0));
    AddScalarParam(&info.params, "rule2_min_trailing_silence",
                   info.recognizer_config.value("rule2_min_trailing_silence",
                                                0.0));
    AddScalarParam(&info.params, "rule3_min_utterance_length",
                   info.recognizer_config.value("rule3_min_utterance_length",
                                                0.0));
    AddScalarParam(&info.params, "hotwords_score",
                   info.recognizer_config.value("hotwords_score", 0.0));
    AddScalarParam(&info.params, "blank_penalty",
                   info.recognizer_config.value("blank_penalty", 0.0));

    AddScalarParam(&info.params, "num_threads",
                   info.model_config.value("num_threads", 0));
    AddScalarParam(&info.params, "debug",
                   info.model_config.value("debug", 0));
    AddScalarParam(&info.params, "provider",
                   info.model_config.value("provider", ""));
    AddScalarParam(&info.params, "model_type",
                   info.model_config.value("model_type", ""));
    AddScalarParam(&info.params, "modeling_unit",
                   info.model_config.value("modeling_unit", ""));

    if (info.model_config.contains(info.family) &&
        info.model_config[info.family].is_object()) {
      const auto &family_config = info.model_config[info.family];
      for (const auto &[key, val] : family_config.items()) {
        if (val.is_string()) {
          ResolveModelFileField(dir, model_root, family_config, key, key, &info);
        }
        AddScalarParam(&info.params, key, val);
      }
    }

  } catch (const std::exception &e) {
    if (error) {
      *error = "failed to parse '" + json_path.string() + "': " + e.what();
    }
  }

  return info;
}

// Check that the tokens file exists (required for all model types)
bool HasTokens(const ModelInfo &info) {
  return !info.File("tokens").empty() && fs::exists(info.File("tokens"));
}

bool HasTokenizer(const ModelInfo &info) {
  return !info.File("tokenizer").empty() && fs::exists(info.File("tokenizer"));
}

bool HasRequiredTextAsset(const ModelInfo &info) {
  return FamilyUsesTokenizerAsset(info.family) ? HasTokenizer(info)
                                               : HasTokens(info);
}

std::string RequiredTextAssetField(const ModelInfo &info) {
  return FamilyUsesTokenizerAsset(info.family) ? "model.tokenizer"
                                               : "model.tokens";
}

std::string RequiredTextAssetPathKey(const ModelInfo &info) {
  return FamilyUsesTokenizerAsset(info.family) ? "tokenizer" : "tokens";
}

// Check that at least one model/encoder file exists
bool HasModelFiles(const ModelInfo &info) {
  for (const auto &[key, path] : info.files) {
    if (key == "tokens") continue;
    if (!path.empty() && fs::exists(path)) return true;
  }
  return false;
}

bool IsDirectoryModelBackend(const ModelInfo &info) {
  return info.backend == "vosk-streaming" || info.backend == "vosk-offline";
}

} // namespace

std::string ModelInfo::RuntimeLanguageHint() const {
  auto looks_like_runtime_hint = [](std::string_view value) {
    if (value.empty()) {
      return false;
    }
    for (char c : value) {
      const bool ok = (c >= 'a' && c <= 'z') || c == '-' || c == '_';
      if (!ok) {
        return false;
      }
    }
    return value != "multilingual" && value.find('_') == std::string_view::npos;
  };

  if (model_config.contains(family) && model_config[family].is_object()) {
    const auto &family_config = model_config[family];
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
fs::path ModelManager::NormalizeBaseDir(const std::string &raw_path) {
  if (raw_path.empty()) {
    return vinput::path::DefaultModelBaseDir();
  }
  return vinput::path::ExpandUserPath(raw_path);
}

fs::path ModelManager::RelativePathForId(std::string_view model_id) {
  const auto segments = SplitModelId(model_id);
  if (segments.empty() || !HasManagedModelPathDepth(segments)) {
    return {};
  }

  std::size_t start_index = 0;
  if (segments.size() > 1 && segments.front() == "model") {
    start_index = 1;
  }
  if (start_index >= segments.size()) {
    return {};
  }

  fs::path relative_path;
  for (std::size_t i = start_index; i < segments.size(); ++i) {
    relative_path /= segments[i];
  }
  return relative_path;
}

std::string
ModelManager::IdFromRelativePath(const std::filesystem::path &relative_path) {
  std::vector<std::string> relative_segments;
  for (const auto &component : relative_path) {
    const std::string part = component.string();
    if (part.empty() || part == "." || part == "..") {
      return {};
    }
    relative_segments.push_back(part);
  }
  if (!HasManagedModelPathDepth(relative_segments)) {
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

ModelManager::ModelManager(const std::string &base_dir,
                           const std::string &model_id) {
  base_dir_ = NormalizeBaseDir(base_dir).string();
  model_id_ = model_id;
}

bool ModelManager::EnsureModels(std::string *error) {
  auto dir = ModelDir(model_id_);
  auto json_path = dir / "vinput-model.json";

  if (!fs::exists(json_path)) {
    if (error) {
      *error = "missing 'vinput-model.json' in " + dir.string();
    }
    return false;
  }

  std::string parse_error;
  auto info = GetModelInfo(&parse_error);
  if (info.family.empty()) {
    if (error) {
      if (!parse_error.empty()) {
        *error = std::move(parse_error);
      } else {
        *error = "'vinput-model.json' is missing family for model '" +
                 model_id_ + "'";
      }
    }
    return false;
  }

  if (!info.rejected_files.empty()) {
    const auto &[key, raw_path] = *info.rejected_files.begin();
    if (error) {
      *error = "'vinput-model.json' contains invalid path for '" + key +
               "': " + raw_path;
    }
    return false;
  }

  if (!IsDirectoryModelBackend(info)) {
    if (!HasRequiredTextAsset(info)) {
      if (error) {
        *error = RequiredTextAssetPathKey(info) +
                 " file not found for model '" + model_id_ + "'";
      }
      return false;
    }

    if (!HasModelFiles(info)) {
      if (error) {
        *error = "no model files found for model '" + model_id_ + "'";
      }
      return false;
    }
  } else {
    const std::string model_path = info.File("model");
    if (model_path.empty() || !fs::exists(model_path) || !fs::is_directory(model_path)) {
      if (error) {
        *error = "vosk model directory not found for model '" + model_id_ + "'";
      }
      return false;
    }
  }

  return true;
}

ModelInfo ModelManager::GetModelInfo(std::string *error) const {
  auto dir = ModelDir(model_id_);
  auto json_path = dir / "vinput-model.json";

  if (!fs::exists(json_path)) {
    return {};
  }

  return ParseModelJson(dir, json_path, error);
}

std::string ModelManager::GetBaseDir() const { return base_dir_; }

std::vector<std::string> ModelManager::ListModels() const {
  std::vector<std::string> models;
  const auto root = fs::path(base_dir_);
  if (!fs::exists(root) || !fs::is_directory(root)) {
    return models;
  }

  for (const auto &entry : fs::directory_iterator(root)) {
    if (entry.is_directory()) {
      for (const auto &nested : fs::recursive_directory_iterator(entry.path())) {
        if (!nested.is_regular_file() ||
            nested.path().filename() != "vinput-model.json") {
          continue;
        }
        std::error_code rel_ec;
        const fs::path relative_path =
            nested.path().parent_path().lexically_relative(root);
        const std::string model_id = IdFromRelativePath(relative_path);
        if (!model_id.empty() && IsValidModelDir(model_id)) {
          models.push_back(model_id);
        }
      }
    }
  }

  std::sort(models.begin(), models.end());
  models.erase(std::unique(models.begin(), models.end()), models.end());
  return models;
}

std::string ModelManager::GetModelId() const { return model_id_; }

bool ModelManager::IsValidModelDir(const std::string &model_id) const {
  const auto dir = ModelDir(model_id);
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

  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().filename() != "vinput-model.json") {
      continue;
    }

    const auto relative_path = entry.path().parent_path().lexically_relative(root);
    const auto model_id = IdFromRelativePath(relative_path);
    if (model_id.empty()) {
      continue;
    }
    ModelSummary s;
    s.id = model_id;

    if (!IsValidModelDir(model_id)) {
      s.state = ModelState::Broken;
      summaries.push_back(std::move(s));
      continue;
    }

    // Parse model summary from vinput-model.json
    const auto json_path = entry.path();
    try {
      std::ifstream file(json_path);
      json j;
      file >> j;
      s.model_type = j.value("family", "");
      s.language = j.value("language", "auto");
      s.supports_hotwords = j.value("supports_hotwords", false);
      s.size_bytes = j.value("size_bytes", uint64_t{0});
    } catch (...) {
      s.state = ModelState::Broken;
      summaries.push_back(std::move(s));
      continue;
    }

    s.state =
        (model_id == active_model) ? ModelState::Active : ModelState::Installed;
    summaries.push_back(std::move(s));
  }

  std::sort(summaries.begin(), summaries.end(),
            [](const ModelSummary &a, const ModelSummary &b) {
              return a.id < b.id;
            });
  return summaries;
}

bool ModelManager::Validate(const std::string &model_id,
                            std::string *error) const {
  const auto dir = ModelDir(model_id);
  if (dir.empty()) {
    if (error) *error = "invalid model id: " + model_id;
    return false;
  }

  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    if (error) *error = "model directory does not exist: " + dir.string();
    return false;
  }

  const auto json_path = dir / "vinput-model.json";
  if (!fs::exists(json_path) || !fs::is_regular_file(json_path)) {
    if (error) *error = "vinput-model.json not found in " + dir.string();
    return false;
  }

  std::string parse_error;
  auto info = ParseModelJson(dir, json_path, &parse_error);

  if (info.family.empty()) {
    if (error) {
      if (!parse_error.empty()) {
        *error = std::move(parse_error);
      } else {
        *error = "vinput-model.json missing required field: family";
      }
    }
    return false;
  }

  if (!info.rejected_files.empty()) {
    const auto &[key, raw_path] = *info.rejected_files.begin();
    if (error) {
      *error = "vinput-model.json contains out-of-bounds file path for '" +
               key + "': " + raw_path;
    }
    return false;
  }

  if (!HasRequiredTextAsset(info)) {
    if (!IsDirectoryModelBackend(info)) {
      const auto path_key = RequiredTextAssetPathKey(info);
      auto asset_path = info.File(path_key);
      if (asset_path.empty()) {
        if (error)
          *error =
              "vinput-model.json missing required field: " +
              RequiredTextAssetField(info);
      } else {
        if (error) *error = path_key + " file not found: " + asset_path;
      }
      return false;
    }

    const std::string model_path = info.File("model");
    if (model_path.empty()) {
      if (error) *error = "vinput-model.json missing required field: model.vosk.model";
      return false;
    }
    if (!fs::exists(model_path) || !fs::is_directory(model_path)) {
      if (error) *error = "vosk model directory not found: " + model_path;
      return false;
    }
  }

  if (!IsDirectoryModelBackend(info) && !HasModelFiles(info)) {
    if (error) *error = "no model/encoder files found in model directory";
    return false;
  }

  return true;
}

bool ModelManager::Remove(const std::string &model_id,
                          std::string *error) const {
  const fs::path requested_dir = ModelDir(model_id);
  if (requested_dir.empty()) {
    if (error) *error = "invalid model id: " + model_id;
    return false;
  }

  std::error_code ec;
  const auto dir = fs::weakly_canonical(requested_dir, ec);
  if (ec) {
    if (error) *error = "failed to resolve model directory: " + ec.message();
    return false;
  }
  const auto base = fs::weakly_canonical(fs::path(base_dir_), ec);
  if (ec) {
    if (error) *error = "failed to resolve model base directory: " + ec.message();
    return false;
  }

  if (!IsPathWithinRoot(dir, base)) {
    if (error) *error = "invalid model id: " + model_id;
    return false;
  }

  if (!fs::exists(dir)) {
    if (error) *error = "model directory does not exist: " + dir.string();
    return false;
  }

  fs::remove_all(dir, ec);
  if (ec) {
    if (error)
      *error = "failed to remove model directory: " + ec.message();
    return false;
  }

  return true;
}
