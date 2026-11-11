#include "common/registry/registry_models.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glaze/core/common.hpp>
#include <glaze/core/opts.hpp>
#include <glaze/core/write.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <system_error>
#include <utility>
#include <vector>

#include "common/asr/bpe_vocab_exporter.h"
#include "common/asr/model_manager.h"
#include "common/json/json_access.h"
#include "common/registry/registry_cache.h"
#include "common/registry/registry_fetch.h"
#include "common/utils/downloader.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace fs = std::filesystem;

// The registry and the emitted model manifest are keyed by runtime ids, so
// their shape stays dynamic.
constexpr auto kManifestWriteOpts = glz::opts{.prettify = uint8_t{1}, .indentation_width = 2};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

std::optional<RemoteModelEntry> ParseRemoteModelEntry(const glz::json_t& item) {
  RemoteModelEntry e;
  e.id = vinput::json::String(item, "id");
  e.short_id = vinput::json::String(item, "short_id");
  e.size_bytes = static_cast<uint64_t>(vinput::json::Int64(item, "size_bytes", 0));
  e.language = vinput::json::String(item, "language");
  for (auto& url : vinput::json::Strings(item, "urls")) {
    if (!url.empty()) {
      e.urls.push_back(std::move(url));
    }
  }
  e.sha256 = vinput::json::String(item, "sha256");
  if (const auto* model = vinput::json::Find(item, "vinput_model")) {
    e.vinput_model = *model;
  }
  if (!e.id.empty() && !e.urls.empty()) {
    return e;
  }
  return std::nullopt;
}

std::vector<RemoteModelEntry> ParseRegistryJson(const std::string& content, std::string* error) {
  std::vector<RemoteModelEntry> entries;

  glz::json_t root;
  if (const auto ec = glz::read_json(root, content); bool(ec)) {
    if (error != nullptr) {
      *error = "failed to parse registry JSON: " + glz::format_error(ec, content);
    }
    return entries;
  }
  const auto* items = vinput::json::Find(root, "items");
  if (items == nullptr) {
    if (error != nullptr) {
      *error = "registry JSON is not an object or is missing array field 'items'";
    }
    return entries;
  }
  const auto* item_array = items->get_if<glz::json_t::array_t>();
  if (item_array == nullptr) {
    if (error != nullptr) {
      *error = "registry JSON is missing array field 'items'";
    }
    return entries;
  }

  entries.reserve(item_array->size());
  for (const auto& item : *item_array) {
    if (auto entry = ParseRemoteModelEntry(item)) {
      entries.push_back(std::move(*entry));
    }
  }

  return entries;
}

bool ResolveInstallAssetPath(const fs::path& root, const std::string& raw_path, fs::path* resolved,
                             std::string* error) {
  const fs::path relative(raw_path);
  if (relative.empty() || relative.is_absolute()) {
    if (error != nullptr) {
      *error = "model asset path must be relative: " + raw_path;
    }
    return false;
  }
  for (const auto& component : relative) {
    if (component == "..") {
      if (error != nullptr) {
        *error = "model asset path escapes install root: " + raw_path;
      }
      return false;
    }
  }
  *resolved = (root / relative).lexically_normal();
  return true;
}

bool LoadEffectiveModelManifest(const fs::path& model_dir, glz::json_t* manifest,
                                std::string* error) {
  const fs::path manifest_path = model_dir / "vinput-model.json";
  std::error_code ec;
  if (!fs::is_regular_file(manifest_path, ec) || ec) {
    *manifest = glz::json_t();
    return true;
  }
  std::ifstream input(manifest_path);
  const std::string content{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
  if (const auto parse_ec = glz::read_json(*manifest, content); bool(parse_ec)) {
    if (error != nullptr) {
      *error =
          "failed to parse effective vinput-model.json: " + glz::format_error(parse_ec, content);
    }
    return false;
  }
  if (!manifest->is_object()) {
    if (error != nullptr) {
      *error = "effective vinput-model.json must be a JSON object";
    }
    return false;
  }
  return true;
}

bool ValidateModelFamilyAndHotwords(const glz::json_t& vinput_model, std::string* error) {
  if (!vinput_model.is_object()) {
    return false;
  }
  const auto* hotwords_flag = vinput::json::Find(vinput_model, "supports_hotwords");
  if (hotwords_flag != nullptr && !hotwords_flag->is_boolean()) {
    if (error != nullptr) {
      *error = "supports_hotwords must be a boolean";
    }
    return false;
  }
  if (!vinput::json::Bool(vinput_model, "supports_hotwords")) {
    return false;
  }
  const auto* family_node = vinput::json::Find(vinput_model, "family");
  if (family_node == nullptr || !family_node->is_string()) {
    if (error != nullptr) {
      *error = "hotword-capable model is missing string field family";
    }
    return false;
  }
  const std::string family = family_node->get<std::string>();
  return family == "transducer" || family == "nemo_transducer";
}

bool ValidateModelingUnit(const glz::json_t& model, std::string* unit_out, std::string* error) {
  const auto* unit_node = vinput::json::Find(model, "modeling_unit");
  if (unit_node == nullptr || !unit_node->is_string()) {
    if (error != nullptr) {
      *error = "hotword-capable transducer requires model.modeling_unit";
    }
    return false;
  }
  const std::string unit = unit_node->get<std::string>();
  if (unit != "cjkchar" && unit != "bpe" && unit != "bbpe" && unit != "cjkchar+bpe") {
    if (error != nullptr) {
      *error = "unsupported hotword modeling_unit: " + unit;
    }
    return false;
  }
  if (unit == "cjkchar") {
    return false;
  }
  *unit_out = unit;
  return true;
}

// Tri-state precondition outcome: models that never need BPE assets are a
// normal skip, manifest contradictions are hard errors.
enum class BpeDecision : std::uint8_t { kSkip, kProvision, kError };

BpeDecision ShouldProvisionBpe(const glz::json_t& vinput_model, const glz::json_t** model_out,
                               std::string* unit_out, std::string* error) {
  // Use a local error buffer so external pointer state (nullness or pre-existing
  // contents) never skews the decision between a normal skip and a hard error.
  std::string local_error;
  const auto rejection = [&] {
    if (local_error.empty()) {
      return BpeDecision::kSkip;
    }
    if (error != nullptr) {
      *error = std::move(local_error);
    }
    return BpeDecision::kError;
  };

  if (!ValidateModelFamilyAndHotwords(vinput_model, &local_error)) {
    return rejection();
  }
  const auto* model_node = vinput::json::Find(vinput_model, "model");
  if (model_node == nullptr || !model_node->is_object()) {
    if (error != nullptr) {
      *error = "hotword-capable transducer is missing object field model";
    }
    return BpeDecision::kError;
  }
  if (!ValidateModelingUnit(*model_node, unit_out, &local_error)) {
    return rejection();
  }
  *model_out = model_node;
  return BpeDecision::kProvision;
}

bool VerifyExistingBpeVocab(const fs::path& model_dir, const glz::json_t& model,
                            const std::string& unit, fs::path* vocab_path_out,
                            bool* already_valid_out, std::string* error) {
  const auto* vocab_node = vinput::json::Find(model, "bpe_vocab");
  if (vocab_node == nullptr || !vocab_node->is_string() || vocab_node->get<std::string>().empty()) {
    if (error != nullptr) {
      *error = "hotword modeling_unit '" + unit + "' requires model.bpe_vocab";
    }
    return false;
  }
  const std::string vocab_field = vocab_node->get<std::string>();
  fs::path vocab_path;
  if (!ResolveInstallAssetPath(model_dir, vocab_field, &vocab_path, error)) {
    return false;
  }
  std::error_code ec;
  if (fs::is_regular_file(vocab_path, ec) && !ec) {
    if (auto valid_res = ValidateBpeVocabulary(vocab_path); !valid_res) {
      if (error != nullptr) {
        *error = valid_res.error();
      }
      return false;
    }
    *already_valid_out = true;
    return true;
  }
  *vocab_path_out = std::move(vocab_path);
  *already_valid_out = false;
  return true;
}

bool GenerateBpeVocabFromModel(const fs::path& model_dir, const glz::json_t& model,
                               const fs::path& vocab_path, std::string* error) {
  const auto* bpe_model_node = vinput::json::Find(model, "bpe_model");
  if (bpe_model_node == nullptr || !bpe_model_node->is_string() ||
      bpe_model_node->get<std::string>().empty()) {
    if (error != nullptr) {
      *error = "BPE vocabulary is missing and model.bpe_model is not declared";
    }
    return false;
  }
  const std::string bpe_model_field = bpe_model_node->get<std::string>();
  fs::path bpe_model_path;
  if (!ResolveInstallAssetPath(model_dir, bpe_model_field, &bpe_model_path, error)) {
    return false;
  }
  std::error_code ec;
  if (!fs::is_regular_file(bpe_model_path, ec) || ec) {
    if (error != nullptr) {
      *error = "SentencePiece model not found: " + bpe_model_path.string();
    }
    return false;
  }
  if (auto export_res = ExportSentencePieceVocabulary(bpe_model_path, vocab_path); !export_res) {
    if (error != nullptr) {
      *error = export_res.error();
    }
    return false;
  }
  if (auto valid_res = ValidateBpeVocabulary(vocab_path); !valid_res) {
    if (error != nullptr) {
      *error = valid_res.error();
    }
    return false;
  }
  return true;
}

bool ProvisionBpeVocabulary(const fs::path& model_dir, const glz::json_t& vinput_model,
                            std::string* error) {
  const glz::json_t* model_node = nullptr;
  std::string unit;
  switch (ShouldProvisionBpe(vinput_model, &model_node, &unit, error)) {
  case BpeDecision::kSkip:
    return true;
  case BpeDecision::kError:
    return false;
  case BpeDecision::kProvision:
    break;
  }

  fs::path vocab_path;
  bool already_valid = false;
  if (!VerifyExistingBpeVocab(model_dir, *model_node, unit, &vocab_path, &already_valid, error)) {
    return false;
  }
  if (already_valid) {
    return true;
  }

  return GenerateBpeVocabFromModel(model_dir, *model_node, vocab_path, error);
}

std::vector<RemoteModelEntry> FetchRegistryImpl(const CoreConfig* config,
                                                const std::vector<std::string>& registry_urls,
                                                std::string* error,
                                                std::string* resolved_registry_url,
                                                std::vector<std::string>* warnings) {
  if (registry_urls.empty()) {
    if (error != nullptr) {
      *error = "no registry URLs configured";
    }
    return {};
  }

  std::string content;
  vinput::download::Options options;
  options.timeout_seconds = 30;
  options.max_bytes = 4ULL * 1024ULL * 1024ULL;

  vinput::download::Result download_result;
  if (!vinput::registry::FetchRegistryText(config, registry_urls,
                                           vinput::registry::cache::ModelRegistryPath(), options,
                                           &content, &download_result, error, warnings)) {
    if (resolved_registry_url != nullptr) {
      resolved_registry_url->clear();
    }
    return {};
  }

  if (resolved_registry_url != nullptr) {
    *resolved_registry_url = download_result.resolved_url;
  }
  return ParseRegistryJson(content, error);
}

} // namespace

// ---------------------------------------------------------------------------
// ModelRepository
// ---------------------------------------------------------------------------

ModelRepository::ModelRepository(const std::string& base_dir) : base_dir_(base_dir) {}

std::vector<RemoteModelEntry> ModelRepository::FetchRegistry(const std::string& registry_url,
                                                             std::string* error) {
  return FetchRegistry(std::vector<std::string>{registry_url}, error, nullptr);
}

std::vector<RemoteModelEntry>
ModelRepository::FetchRegistry(const std::vector<std::string>& registry_urls, std::string* error,
                               std::string* resolved_registry_url) {
  return FetchRegistryImpl(nullptr, registry_urls, error, resolved_registry_url, nullptr);
}

std::vector<RemoteModelEntry> ModelRepository::FetchRegistry(
    const CoreConfig& config, const std::vector<std::string>& registry_urls, std::string* error,
    std::string* resolved_registry_url, std::vector<std::string>* warnings) {
  return FetchRegistryImpl(&config, registry_urls, error, resolved_registry_url, warnings);
}

bool ModelRepository::InstallModel(const std::string& registry_url, const std::string& model_id,
                                   const ProgressCallback& progress_cb, std::string* error,
                                   const std::atomic<bool>* cancel_token) const {
  return InstallModelImpl(nullptr, std::vector<std::string>{registry_url}, model_id, progress_cb,
                          error, nullptr, cancel_token);
}

bool ModelRepository::InstallModelImpl(const CoreConfig* config,
                                       const std::vector<std::string>& registry_urls,
                                       const std::string& model_id,
                                       const ProgressCallback& progress_cb, std::string* error,
                                       std::string* resolved_registry_url,
                                       const std::atomic<bool>* cancel_token) const {
  std::string fetch_err;
  auto entries =
      FetchRegistryImpl(config, registry_urls, &fetch_err, resolved_registry_url, nullptr);
  if (!fetch_err.empty()) {
    if (error != nullptr) {
      *error = fetch_err;
    }
    return false;
  }

  const auto it = std::ranges::find_if(entries, [&](const auto& e) { return e.id == model_id; });
  if (it == entries.end()) {
    if (error != nullptr) {
      *error = "model not found in registry: " + model_id;
    }
    return false;
  }

  if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
    if (error != nullptr) {
      *error = "download cancelled";
    }
    return false;
  }

  return InstallModelEntry(*it, progress_cb, error, cancel_token);
}

bool ModelRepository::InstallModel(const std::vector<std::string>& registry_urls,
                                   const std::string& model_id, const ProgressCallback& progress_cb,
                                   std::string* error, std::string* resolved_registry_url,
                                   const std::atomic<bool>* cancel_token) const {
  return InstallModelImpl(nullptr, registry_urls, model_id, progress_cb, error,
                          resolved_registry_url, cancel_token);
}

bool ModelRepository::InstallModel(const CoreConfig& config,
                                   const std::vector<std::string>& registry_urls,
                                   const std::string& model_id, const ProgressCallback& progress_cb,
                                   std::string* error, std::string* resolved_registry_url,
                                   const std::atomic<bool>* cancel_token) const {
  return InstallModelImpl(&config, registry_urls, model_id, progress_cb, error,
                          resolved_registry_url, cancel_token);
}

namespace {

std::string DetermineArchiveFilename(const RemoteModelEntry& found) {
  std::string archive_name = found.id + ".tar.gz";
  if (!found.urls.empty()) {
    auto pos = found.urls[0].rfind('/');
    if (pos != std::string::npos && pos + 1 < found.urls[0].size()) {
      archive_name = found.urls[0].substr(pos + 1);
      auto q = archive_name.find('?');
      if (q != std::string::npos) {
        archive_name = archive_name.substr(0, q);
      }
    }
  }
  return archive_name;
}

bool DownloadAndVerifyModelArchive(const RemoteModelEntry& found, const fs::path& archive_path,
                                   const ProgressCallback& progress_cb,
                                   const std::atomic<bool>* cancel_token, std::string* error) {
  vinput::download::Options download_options;
  download_options.timeout_seconds = 600;
  download_options.cancel_token = cancel_token;
  download_options.progress_cb = [progress_cb](const vinput::download::Progress& progress) {
    if (!progress_cb) {
      return;
    }
    InstallProgress install_progress;
    install_progress.downloaded_bytes = progress.downloaded_bytes;
    install_progress.total_bytes = progress.total_bytes;
    install_progress.speed_bps = progress.speed_bps;
    progress_cb(install_progress);
  };
  vinput::download::Result download_result;
  if (!vinput::download::DownloadFile(found.urls, archive_path, download_options,
                                      &download_result)) {
    if (error != nullptr) {
      *error = download_result.error.empty() ? "download failed" : download_result.error;
    }
    return false;
  }

  if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
    if (error != nullptr) {
      *error = "download cancelled";
    }
    return false;
  }

  return ModelRepository::VerifySha256(archive_path, found.sha256, error);
}

void FlattenSingleTopDirectory(const fs::path& extracted_dir, const fs::path& tmp_dir) {
  std::vector<fs::path> top_entries;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(extracted_dir, ec)) {
    top_entries.push_back(entry.path());
  }
  if (top_entries.size() == 1 && fs::is_directory(top_entries[0])) {
    const fs::path& single_dir = top_entries[0];
    const fs::path flatten_tmp = tmp_dir / "flatten";
    fs::rename(single_dir, flatten_tmp, ec);
    if (!ec) {
      for (const auto& entry : fs::directory_iterator(flatten_tmp, ec)) {
        fs::rename(entry.path(), extracted_dir / entry.path().filename(), ec);
      }
      fs::remove_all(flatten_tmp, ec);
    }
  }
}

bool WriteModelMetadataFile(const fs::path& extracted_dir, const glz::json_t& vinput_model,
                            std::string* error) {
  if (vinput_model.is_null() || vinput_model.empty()) {
    return true;
  }
  const fs::path json_path = extracted_dir / "vinput-model.json";
  if (!vinput_model.is_object()) {
    if (error != nullptr) {
      *error = "registry field 'vinput_model' must be a JSON object";
    }
    return false;
  }
  std::string json_content;
  if (bool(glz::write<kManifestWriteOpts>(vinput_model, json_content))) {
    if (error != nullptr) {
      *error = "failed to serialize registry model metadata";
    }
    return false;
  }
  json_content += "\n";
  if (auto res = vinput::file::AtomicWriteTextFile(json_path, json_content); !res) {
    if (error != nullptr) {
      *error = "failed to write vinput-model.json: " + res.error();
    }
    return false;
  }
  return true;
}

bool WriteModelMetadataAndProvision(const fs::path& extracted_dir, const glz::json_t& vinput_model,
                                    std::string* error) {
  if (!WriteModelMetadataFile(extracted_dir, vinput_model, error)) {
    return false;
  }

  glz::json_t effective_manifest;
  std::string provision_error;
  if (!LoadEffectiveModelManifest(extracted_dir, &effective_manifest, &provision_error) ||
      !ProvisionBpeVocabulary(extracted_dir, effective_manifest, &provision_error)) {
    if (error != nullptr) {
      *error = "failed to prepare hotword assets: " + provision_error;
    }
    return false;
  }
  return true;
}

class ModelInstallationTransaction {
public:
  static std::optional<ModelInstallationTransaction>
  Create(const fs::path& base_dir, fs::path dest_dir, bool had_existing, std::string* error) {
    auto temp_dir = vinput::file::ScopedTempDir::Create(base_dir, ".tmp-", error);
    if (!temp_dir) {
      return std::nullopt;
    }
    return ModelInstallationTransaction(std::move(temp_dir), std::move(dest_dir), had_existing);
  }

  [[nodiscard]] const fs::path& TempDir() const { return temp_dir_->Path(); }
  [[nodiscard]] fs::path ArchivePath(const RemoteModelEntry& found) const {
    return TempDir() / DetermineArchiveFilename(found);
  }
  [[nodiscard]] fs::path ExtractedDir() const { return TempDir() / "extracted"; }
  [[nodiscard]] fs::path BackupDir() const { return TempDir() / "previous-install"; }

  bool Commit(std::string* error) {
    std::error_code ec;
    if (had_existing_) {
      fs::rename(dest_dir_, BackupDir(), ec);
      if (ec) {
        if (error != nullptr) {
          *error = "failed to stage existing model for replacement: " + ec.message();
        }
        return false;
      }
      staged_backup_ = true;
    }

    fs::create_directories(dest_dir_.parent_path(), ec);
    if (ec) {
      return FailAndRollback("failed to create model parent directory: " + ec.message(), error);
    }

    fs::rename(ExtractedDir(), dest_dir_, ec);
    if (ec) {
      return FailAndRollback("failed to activate new model installation: " + ec.message(), error);
    }

    committed_ = true;
    return true;
  }

  ~ModelInstallationTransaction() {
    if (staged_backup_ && !committed_) {
      std::error_code rollback_ec;
      fs::rename(BackupDir(), dest_dir_, rollback_ec);
      if (rollback_ec) {
        temp_dir_->Dismiss();
      }
    }
  }

  ModelInstallationTransaction(ModelInstallationTransaction&&) noexcept = default;
  ModelInstallationTransaction& operator=(ModelInstallationTransaction&&) = delete;
  ModelInstallationTransaction(const ModelInstallationTransaction&) = delete;
  ModelInstallationTransaction& operator=(const ModelInstallationTransaction&) = delete;

private:
  ModelInstallationTransaction(std::unique_ptr<vinput::file::ScopedTempDir> temp_dir,
                               fs::path dest_dir, bool had_existing)
      : temp_dir_(std::move(temp_dir)), dest_dir_(std::move(dest_dir)),
        had_existing_(had_existing) {}

  bool FailAndRollback(std::string_view reason, std::string* error) {
    if (had_existing_) {
      std::error_code rollback_ec;
      fs::rename(BackupDir(), dest_dir_, rollback_ec);
      if (rollback_ec) {
        temp_dir_->Dismiss();
        if (error != nullptr) {
          *error = std::string(reason) + "; rollback also failed: " + rollback_ec.message() +
                   "; previous installation preserved at: " + BackupDir().string();
        }
        return false;
      }
      staged_backup_ = false;
    }
    if (error != nullptr) {
      *error = std::string(reason);
    }
    return false;
  }

  std::unique_ptr<vinput::file::ScopedTempDir> temp_dir_;
  fs::path dest_dir_;
  bool had_existing_ = false;
  bool staged_backup_ = false;
  bool committed_ = false;
};

bool ExtractAndFlattenModelArchive(const fs::path& archive_path, const fs::path& extracted_dir,
                                   const fs::path& tmp_dir, std::string* error) {
  std::error_code ec;
  fs::create_directories(extracted_dir, ec);
  if (!ModelRepository::ExtractArchive(archive_path, extracted_dir, error)) {
    return false;
  }
  FlattenSingleTopDirectory(extracted_dir, tmp_dir);
  return true;
}

std::optional<fs::path> ResolveModelDestinationDir(const fs::path& base_dir,
                                                   const std::string& model_id, bool* had_existing,
                                                   std::string* error) {
  const fs::path relative_path = ModelManager::RelativePathForId(model_id);
  if (relative_path.empty()) {
    if (error != nullptr) {
      *error = "invalid model id: " + model_id;
    }
    return std::nullopt;
  }
  fs::path dest_dir = base_dir / relative_path;
  std::error_code exists_ec;
  *had_existing = fs::exists(dest_dir, exists_ec);
  if (exists_ec) {
    if (error != nullptr) {
      *error = "failed to inspect existing model installation: " + exists_ec.message();
    }
    return std::nullopt;
  }
  return dest_dir;
}

bool IsArchiveEntryPathSafe(std::string_view entry_path, std::string* error) {
  if (!entry_path.empty() && entry_path[0] == '/') {
    if (error != nullptr) {
      *error = "archive contains absolute path: " + std::string(entry_path);
    }
    return false;
  }
  const fs::path check_path(entry_path);
  if (std::ranges::any_of(check_path, [](const auto& component) { return component == ".."; })) {
    if (error != nullptr) {
      *error = "archive contains path traversal component: " + std::string(entry_path);
    }
    return false;
  }
  return true;
}

bool ValidateArchiveEntryType(mode_t entry_type, struct archive_entry* entry,
                              std::string_view entry_path, std::string* error) {
  if (archive_entry_symlink(entry) != nullptr || archive_entry_hardlink(entry) != nullptr) {
    if (error != nullptr) {
      *error = "archive contains link entry: " + std::string(entry_path);
    }
    return false;
  }
  if (entry_type != AE_IFREG && entry_type != AE_IFDIR) {
    if (error != nullptr) {
      *error = "archive contains unsupported entry type: " + std::string(entry_path);
    }
    return false;
  }
  return true;
}

bool ValidateArchiveEntryPath(const char* raw_path, mode_t entry_type, struct archive_entry* entry,
                              std::string* error) {
  if (raw_path == nullptr) {
    return false;
  }
  const std::string_view entry_path(raw_path);
  if (!IsArchiveEntryPathSafe(entry_path, error)) {
    return false;
  }
  return ValidateArchiveEntryType(entry_type, entry, entry_path, error);
}

bool PrepareArchiveDestination(const fs::path& full_dest, mode_t entry_type,
                               const fs::path& dest_root, std::string_view entry_path,
                               std::string* error) {
  std::error_code create_ec;
  fs::path check_path = full_dest;
  if (entry_type == AE_IFDIR) {
    fs::create_directories(full_dest, create_ec);
  } else {
    fs::create_directories(full_dest.parent_path(), create_ec);
    check_path = full_dest.parent_path();
  }
  if (create_ec) {
    if (error != nullptr) {
      *error = "failed to prepare extraction path: " + create_ec.message();
    }
    return false;
  }
  if (!vinput::path::IsPathWithinRoot(fs::weakly_canonical(check_path), dest_root)) {
    if (error != nullptr) {
      *error = "archive entry escapes extraction root: " + std::string(entry_path);
    }
    return false;
  }
  return true;
}

bool CopyArchiveDataBlocks(struct archive* a, struct archive* out, std::string* error) {
  const void* buff = nullptr;
  size_t size = 0;
  la_int64_t offset = 0;
  int r = 0;
  while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
    if (archive_write_data_block(out, buff, size, offset) != ARCHIVE_OK) {
      if (error != nullptr) {
        *error = std::string("failed to write data: ") + archive_error_string(out);
      }
      return false;
    }
  }
  if (r != ARCHIVE_EOF) {
    if (error != nullptr) {
      *error = std::string("archive read error: ") + archive_error_string(a);
    }
    return false;
  }
  return true;
}

bool ExtractSingleArchiveEntry(struct archive* a, struct archive* out, struct archive_entry* entry,
                               const fs::path& dest, const fs::path& dest_root,
                               std::string* error) {
  const char* raw_path = archive_entry_pathname(entry);
  if (raw_path == nullptr) {
    return true;
  }
  const mode_t entry_type = archive_entry_filetype(entry);
  if (!ValidateArchiveEntryPath(raw_path, entry_type, entry, error)) {
    return false;
  }
  const std::string entry_path(raw_path);
  const fs::path full_dest = dest / entry_path;
  if (!PrepareArchiveDestination(full_dest, entry_type, dest_root, entry_path, error)) {
    return false;
  }

  archive_entry_set_pathname(entry, full_dest.c_str());
  if (archive_write_header(out, entry) != ARCHIVE_OK) {
    if (error != nullptr) {
      *error = std::string("failed to write header: ") + archive_error_string(out);
    }
    return false;
  }

  if (archive_entry_size(entry) > 0) {
    if (!CopyArchiveDataBlocks(a, out, error)) {
      return false;
    }
  }
  archive_write_finish_entry(out);
  return true;
}

} // namespace

bool ModelRepository::InstallModelEntry(const RemoteModelEntry& found,
                                        const ProgressCallback& progress_cb, std::string* error,
                                        const std::atomic<bool>* cancel_token) const {
  bool had_existing = false;
  auto dest_dir = ResolveModelDestinationDir(base_dir_, found.id, &had_existing, error);
  if (!dest_dir) {
    return false;
  }

  auto tx =
      ModelInstallationTransaction::Create(base_dir_, std::move(*dest_dir), had_existing, error);
  if (!tx) {
    return false;
  }

  const fs::path archive_path = tx->ArchivePath(found);
  if (!DownloadAndVerifyModelArchive(found, archive_path, progress_cb, cancel_token, error)) {
    return false;
  }

  const fs::path extracted_dir = tx->ExtractedDir();
  if (!ExtractAndFlattenModelArchive(archive_path, extracted_dir, tx->TempDir(), error)) {
    return false;
  }

  if (!WriteModelMetadataAndProvision(extracted_dir, found.vinput_model, error)) {
    return false;
  }

  if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
    if (error != nullptr) {
      *error = "download cancelled";
    }
    return false;
  }

  return tx->Commit(error);
}

bool ModelRepository::VerifySha256(const fs::path& file, const std::string& expected,
                                   std::string* error) {
  if (expected.empty()) {
    return true;
  }

  const auto hex = vinput::file::ComputeFileSha256(file);
  if (!hex) {
    if (error != nullptr) {
      *error = hex.error();
    }
    return false;
  }

  if (*hex != expected) {
    if (error != nullptr) {
      *error = "SHA256 mismatch: expected " + expected + ", got " + *hex;
    }
    return false;
  }

  return true;
}

bool ModelRepository::ExtractArchive(const fs::path& archive, const fs::path& dest,
                                     std::string* error) {
  struct archive* a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);

  struct archive* out = archive_write_disk_new();
  archive_write_disk_set_options(out, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                          ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
  archive_write_disk_set_standard_lookup(out);

  int r = archive_read_open_filename(a, archive.c_str(), 16384);
  if (r != ARCHIVE_OK) {
    if (error != nullptr) {
      *error = std::string("failed to open archive: ") + archive_error_string(a);
    }
    archive_read_free(a);
    archive_write_free(out);
    return false;
  }

  const fs::path dest_root = fs::weakly_canonical(dest);
  bool success = true;
  struct archive_entry* entry = nullptr;
  while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    if (!ExtractSingleArchiveEntry(a, out, entry, dest, dest_root, error)) {
      success = false;
      break;
    }
  }

  if (success && r != ARCHIVE_EOF) {
    if (error != nullptr) {
      *error = std::string("archive iteration error: ") + archive_error_string(a);
    }
    success = false;
  }

  archive_read_close(a);
  archive_read_free(a);
  archive_write_close(out);
  archive_write_free(out);

  return success;
}
