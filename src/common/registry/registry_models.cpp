#include "common/registry/registry_models.h"

#include <archive.h>
#include <archive_entry.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <string>
#include <vector>

#include "common/asr/bpe_vocab_exporter.h"
#include "common/asr/model_manager.h"
#include "common/registry/registry_cache.h"
#include "common/registry/registry_fetch.h"
#include "common/utils/downloader.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

std::vector<RemoteModelEntry> ParseRegistryJson(const std::string& content, std::string* error) {
  std::vector<RemoteModelEntry> entries;

  try {
    json j = json::parse(content);
    if (!j.is_object()) {
      if (error)
        *error = "registry JSON is not an object";
      return entries;
    }
    if (!j.contains("items") || !j.at("items").is_array()) {
      if (error)
        *error = "registry JSON is missing array field 'items'";
      return entries;
    }
    for (const auto& item : j.at("items")) {
      RemoteModelEntry e;
      e.id = item.value("id", "");
      e.short_id = item.value("short_id", "");
      e.size_bytes = item.value("size_bytes", uint64_t{0});
      e.language = item.value("language", "");
      if (item.contains("urls") && item.at("urls").is_array()) {
        for (const auto& elem : item.at("urls")) {
          if (elem.is_string() && !elem.get<std::string>().empty()) {
            e.urls.push_back(elem.get<std::string>());
          }
        }
      }
      e.sha256 = item.value("sha256", "");
      if (item.contains("vinput_model")) {
        e.vinput_model = item["vinput_model"];
      }
      if (!e.id.empty() && !e.urls.empty()) {
        entries.push_back(std::move(e));
      }
    }
  } catch (const std::exception& ex) {
    if (error)
      *error = std::string("failed to parse registry JSON: ") + ex.what();
    return entries;
  }

  return entries;
}

bool ResolveInstallAssetPath(const fs::path& root, const std::string& raw_path, fs::path* resolved,
                             std::string* error) {
  const fs::path relative(raw_path);
  if (relative.empty() || relative.is_absolute()) {
    if (error)
      *error = "model asset path must be relative: " + raw_path;
    return false;
  }
  for (const auto& component : relative) {
    if (component == "..") {
      if (error)
        *error = "model asset path escapes install root: " + raw_path;
      return false;
    }
  }
  *resolved = (root / relative).lexically_normal();
  return true;
}

bool LoadEffectiveModelManifest(const fs::path& model_dir, json* manifest, std::string* error) {
  const fs::path manifest_path = model_dir / "vinput-model.json";
  std::error_code ec;
  if (!fs::is_regular_file(manifest_path, ec) || ec) {
    *manifest = json();
    return true;
  }
  try {
    std::ifstream input(manifest_path);
    input >> *manifest;
  } catch (const std::exception& ex) {
    if (error) {
      *error = "failed to parse effective vinput-model.json: " + std::string(ex.what());
    }
    return false;
  }
  if (!manifest->is_object()) {
    if (error)
      *error = "effective vinput-model.json must be a JSON object";
    return false;
  }
  return true;
}

bool ProvisionBpeVocabulary(const fs::path& model_dir, const json& vinput_model,
                            std::string* error) {
  if (!vinput_model.is_object())
    return true;
  if (vinput_model.contains("supports_hotwords") &&
      !vinput_model["supports_hotwords"].is_boolean()) {
    if (error)
      *error = "supports_hotwords must be a boolean";
    return false;
  }
  if (!vinput_model.value("supports_hotwords", false))
    return true;
  if (!vinput_model.contains("family") || !vinput_model["family"].is_string()) {
    if (error)
      *error = "hotword-capable model is missing string field family";
    return false;
  }
  const std::string family = vinput_model["family"].get<std::string>();
  if (family != "transducer" && family != "nemo_transducer")
    return true;
  if (!vinput_model.contains("model") || !vinput_model["model"].is_object()) {
    if (error)
      *error = "hotword-capable transducer is missing object field model";
    return false;
  }
  const auto& model = vinput_model["model"];
  if (!model.contains("modeling_unit") || !model["modeling_unit"].is_string()) {
    if (error)
      *error = "hotword-capable transducer requires model.modeling_unit";
    return false;
  }
  const std::string unit = model["modeling_unit"].get<std::string>();
  if (unit != "cjkchar" && unit != "bpe" && unit != "bbpe" && unit != "cjkchar+bpe") {
    if (error)
      *error = "unsupported hotword modeling_unit: " + unit;
    return false;
  }
  if (unit == "cjkchar")
    return true;

  if (!model.contains("bpe_vocab") || !model["bpe_vocab"].is_string() ||
      model["bpe_vocab"].get<std::string>().empty()) {
    if (error) {
      *error = "hotword modeling_unit '" + unit + "' requires model.bpe_vocab";
    }
    return false;
  }
  const std::string vocab_field = model["bpe_vocab"].get<std::string>();
  fs::path vocab_path;
  if (!ResolveInstallAssetPath(model_dir, vocab_field, &vocab_path, error)) {
    return false;
  }
  std::error_code ec;
  if (fs::is_regular_file(vocab_path, ec) && !ec) {
    return ValidateBpeVocabulary(vocab_path, error);
  }

  if (!model.contains("bpe_model") || !model["bpe_model"].is_string() ||
      model["bpe_model"].get<std::string>().empty()) {
    if (error) {
      *error = "BPE vocabulary is missing and model.bpe_model is not declared";
    }
    return false;
  }
  const std::string bpe_model_field = model["bpe_model"].get<std::string>();
  fs::path bpe_model_path;
  if (!ResolveInstallAssetPath(model_dir, bpe_model_field, &bpe_model_path, error)) {
    return false;
  }
  if (!fs::is_regular_file(bpe_model_path, ec) || ec) {
    if (error) {
      *error = "SentencePiece model not found: " + bpe_model_path.string();
    }
    return false;
  }
  if (!ExportSentencePieceVocabulary(bpe_model_path, vocab_path, error)) {
    return false;
  }
  return ValidateBpeVocabulary(vocab_path, error);
}

std::vector<RemoteModelEntry> FetchRegistryImpl(const CoreConfig* config,
                                                const std::vector<std::string>& registry_urls,
                                                std::string* error,
                                                std::string* resolved_registry_url,
                                                std::vector<std::string>* warnings) {
  if (registry_urls.empty()) {
    if (error)
      *error = "no registry URLs configured";
    return {};
  }

  std::string content;
  vinput::download::Options options;
  options.timeout_seconds = 30;
  options.max_bytes = 4 * 1024 * 1024;

  vinput::download::Result download_result;
  if (!vinput::registry::FetchRegistryText(config, registry_urls,
                                           vinput::registry::cache::ModelRegistryPath(), options,
                                           &content, &download_result, error, warnings)) {
    if (resolved_registry_url) {
      resolved_registry_url->clear();
    }
    return {};
  }

  if (resolved_registry_url) {
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
                                                             std::string* error) const {
  return FetchRegistry(std::vector<std::string>{registry_url}, error, nullptr);
}

std::vector<RemoteModelEntry>
ModelRepository::FetchRegistry(const std::vector<std::string>& registry_urls, std::string* error,
                               std::string* resolved_registry_url) const {
  return FetchRegistryImpl(nullptr, registry_urls, error, resolved_registry_url, nullptr);
}

std::vector<RemoteModelEntry> ModelRepository::FetchRegistry(
    const CoreConfig& config, const std::vector<std::string>& registry_urls, std::string* error,
    std::string* resolved_registry_url, std::vector<std::string>* warnings) const {
  return FetchRegistryImpl(&config, registry_urls, error, resolved_registry_url, warnings);
}

bool ModelRepository::InstallModel(const std::string& registry_url, const std::string& model_id,
                                   ProgressCallback progress_cb, std::string* error,
                                   const std::atomic<bool>* cancel_token) const {
  return InstallModel(std::vector<std::string>{registry_url}, model_id, std::move(progress_cb),
                      error, nullptr, cancel_token);
}

bool ModelRepository::InstallModel(const std::vector<std::string>& registry_urls,
                                   const std::string& model_id, ProgressCallback progress_cb,
                                   std::string* error, std::string* resolved_registry_url,
                                   const std::atomic<bool>* cancel_token) const {
  // Fetch registry without i18n refresh when config context is unavailable.
  std::string fetch_err;
  auto entries =
      FetchRegistryImpl(nullptr, registry_urls, &fetch_err, resolved_registry_url, nullptr);
  if (!fetch_err.empty()) {
    if (error != nullptr) {
      *error = fetch_err;
    }
    return false;
  }

  // Find the requested model
  const RemoteModelEntry* found = nullptr;
  for (const auto& e : entries) {
    if (e.id == model_id) {
      found = &e;
      break;
    }
  }
  if (!found) {
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

  return InstallModelEntry(*found, progress_cb, error, cancel_token);
}

bool ModelRepository::InstallModel(const CoreConfig& config,
                                   const std::vector<std::string>& registry_urls,
                                   const std::string& model_id, ProgressCallback progress_cb,
                                   std::string* error, std::string* resolved_registry_url,
                                   const std::atomic<bool>* cancel_token) const {
  // Fetch registry
  std::string fetch_err;
  auto entries = FetchRegistry(config, registry_urls, &fetch_err, resolved_registry_url);
  if (!fetch_err.empty()) {
    if (error != nullptr) {
      *error = fetch_err;
    }
    return false;
  }

  // Find the requested model
  const RemoteModelEntry* found = nullptr;
  for (const auto& e : entries) {
    if (e.id == model_id) {
      found = &e;
      break;
    }
  }
  if (!found) {
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

  return InstallModelEntry(*found, progress_cb, error, cancel_token);
}

bool ModelRepository::InstallModelEntry(const RemoteModelEntry& found,
                                        const ProgressCallback& progress_cb, std::string* error,
                                        const std::atomic<bool>* cancel_token) const {
  auto temp_dir = vinput::file::ScopedTempDir::Create(base_dir_, ".tmp-", error);
  if (!temp_dir) {
    return false;
  }
  const fs::path& tmp_dir = temp_dir->Path();
  std::error_code ec;

  // Determine archive filename from first URL
  std::string archive_name = found.id + ".tar.gz";
  if (!found.urls.empty()) {
    auto pos = found.urls[0].rfind('/');
    if (pos != std::string::npos && pos + 1 < found.urls[0].size()) {
      archive_name = found.urls[0].substr(pos + 1);
      // Strip query string if present
      auto q = archive_name.find('?');
      if (q != std::string::npos) {
        archive_name = archive_name.substr(0, q);
      }
    }
  }

  const fs::path archive_path = tmp_dir / archive_name;

  // Try each URL in order (fallback on failure)
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

  // Verify SHA256 (stub)
  std::string verify_err;
  if (!VerifySha256(archive_path, found.sha256, &verify_err)) {
    if (error != nullptr) {
      *error = verify_err;
    }
    return false;
  }

  // Extract
  const fs::path extracted_dir = tmp_dir / "extracted";
  fs::create_directories(extracted_dir, ec);
  std::string extract_err;
  if (!ExtractArchive(archive_path, extracted_dir, &extract_err)) {
    if (error != nullptr) {
      *error = extract_err;
    }
    return false;
  }

  // If archive extracted into a single top-level directory, flatten it
  // (equivalent to tar --strip-components=1)
  {
    std::vector<fs::path> top_entries;
    for (const auto& entry : fs::directory_iterator(extracted_dir)) {
      top_entries.push_back(entry.path());
    }
    if (top_entries.size() == 1 && fs::is_directory(top_entries[0])) {
      const fs::path single_dir = top_entries[0];
      const fs::path flatten_tmp = tmp_dir / "flatten";
      fs::rename(single_dir, flatten_tmp, ec);
      if (!ec) {
        // Move all contents from flatten_tmp into extracted_dir
        for (const auto& entry : fs::directory_iterator(flatten_tmp)) {
          fs::rename(entry.path(), extracted_dir / entry.path().filename(), ec);
        }
        fs::remove_all(flatten_tmp, ec);
      }
    }
  }

  // Write vinput-model.json if provided
  if (!found.vinput_model.is_null() && !found.vinput_model.empty()) {
    const fs::path json_path = extracted_dir / "vinput-model.json";
    if (!found.vinput_model.is_object()) {
      if (error != nullptr) {
        *error = "registry field 'vinput_model' must be a JSON object";
      }
      return false;
    }
    const std::string json_content = found.vinput_model.dump(2) + "\n";
    std::string write_err;
    if (!vinput::file::AtomicWriteTextFile(json_path, json_content, &write_err)) {
      if (error != nullptr) {
        *error = "failed to write vinput-model.json: " + write_err;
      }
      return false;
    }
  }

  json effective_manifest;
  std::string provision_error;
  if (!LoadEffectiveModelManifest(extracted_dir, &effective_manifest, &provision_error) ||
      !ProvisionBpeVocabulary(extracted_dir, effective_manifest, &provision_error)) {
    if (error != nullptr) {
      *error = "failed to prepare hotword assets: " + provision_error;
    }
    return false;
  }

  // Replace the destination by rename so the old model stays intact until
  // the new one is fully extracted and ready.
  const fs::path relative_path = ModelManager::RelativePathForId(found.id);
  if (relative_path.empty()) {
    if (error != nullptr) {
      *error = "invalid model id: " + found.id;
    }
    return false;
  }
  const fs::path dest_dir = base_dir_ / relative_path;
  const fs::path backup_dir = tmp_dir / "previous-install";
  std::error_code exists_ec;
  const bool had_existing = fs::exists(dest_dir, exists_ec);
  if (exists_ec) {
    if (error != nullptr) {
      *error = "failed to inspect existing model installation: " + exists_ec.message();
    }
    return false;
  }

  if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed)) {
    if (error != nullptr) {
      *error = "download cancelled";
    }
    return false;
  }

  if (had_existing) {
    fs::rename(dest_dir, backup_dir, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "failed to stage existing model for replacement: " + ec.message();
      }
      return false;
    }
  }

  fs::create_directories(dest_dir.parent_path(), ec);
  if (ec) {
    if (had_existing) {
      std::error_code rollback_ec;
      fs::rename(backup_dir, dest_dir, rollback_ec);
      if (rollback_ec) {
        temp_dir->Dismiss();
        if (error != nullptr) {
          *error = "failed to create model parent directory: " + ec.message() +
                   "; rollback also failed: " + rollback_ec.message() +
                   "; previous installation preserved at: " + backup_dir.string();
        }
        return false;
      }
    }
    if (error != nullptr) {
      *error = "failed to create model parent directory: " + ec.message();
    }
    return false;
  }

  fs::rename(extracted_dir, dest_dir, ec);
  if (ec) {
    std::string install_error = "failed to activate new model installation: " + ec.message();
    if (had_existing) {
      std::error_code rollback_ec;
      fs::rename(backup_dir, dest_dir, rollback_ec);
      if (rollback_ec) {
        temp_dir->Dismiss();
        if (error != nullptr) {
          *error = install_error + "; rollback also failed: " + rollback_ec.message() +
                   "; previous installation preserved at: " + backup_dir.string();
        }
        return false;
      }
    }
    if (error != nullptr) {
      *error = install_error;
    }
    return false;
  }

  // Clean up temp dir via RAII ScopedTempDir
  return true;
}

bool ModelRepository::VerifySha256(const fs::path& file, const std::string& expected,
                                   std::string* error) const {
  if (expected.empty())
    return true;

  std::ifstream in(file, std::ios::binary);
  if (!in.is_open()) {
    if (error)
      *error = "failed to open file for SHA256 verification: " + file.string();
    return false;
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    if (error)
      *error = "failed to create EVP_MD_CTX";
    return false;
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    if (error)
      *error = "EVP_DigestInit_ex failed";
    return false;
  }

  char buf[8192];
  while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
    if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(in.gcount())) != 1) {
      EVP_MD_CTX_free(ctx);
      if (error)
        *error = "EVP_DigestUpdate failed";
      return false;
    }
  }

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len = 0;
  if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
    EVP_MD_CTX_free(ctx);
    if (error)
      *error = "EVP_DigestFinal_ex failed";
    return false;
  }
  EVP_MD_CTX_free(ctx);

  // Convert to hex string
  std::string hex;
  hex.reserve(hash_len * 2);
  for (unsigned int i = 0; i < hash_len; ++i) {
    char pair[3];
    std::snprintf(pair, sizeof(pair), "%02x", hash[i]);
    hex += pair;
  }

  if (hex != expected) {
    if (error)
      *error = "SHA256 mismatch: expected " + expected + ", got " + hex;
    return false;
  }

  return true;
}

bool ModelRepository::ExtractArchive(const fs::path& archive, const fs::path& dest,
                                     std::string* error) const {
  struct archive* a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);

  struct archive* out = archive_write_disk_new();
  archive_write_disk_set_options(out, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                          ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
  archive_write_disk_set_standard_lookup(out);

  int r = archive_read_open_filename(a, archive.c_str(), 16384);
  if (r != ARCHIVE_OK) {
    if (error)
      *error = std::string("failed to open archive: ") + archive_error_string(a);
    archive_read_free(a);
    archive_write_free(out);
    return false;
  }

  const fs::path dest_root = fs::weakly_canonical(dest);
  bool success = true;
  struct archive_entry* entry;
  while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    const char* raw_path = archive_entry_pathname(entry);
    if (!raw_path) {
      continue;
    }

    std::string entry_path(raw_path);
    const mode_t entry_type = archive_entry_filetype(entry);

    // Security: reject absolute paths
    if (!entry_path.empty() && entry_path[0] == '/') {
      if (error)
        *error = "archive contains absolute path: " + entry_path;
      success = false;
      break;
    }

    // Security: reject path traversal components
    {
      fs::path check_path(entry_path);
      bool has_dotdot = false;
      for (const auto& component : check_path) {
        if (component == "..") {
          has_dotdot = true;
          break;
        }
      }
      if (has_dotdot) {
        if (error)
          *error = "archive contains path traversal component: " + entry_path;
        success = false;
        break;
      }
    }

    const fs::path full_dest = dest / entry_path;
    if (archive_entry_symlink(entry) != nullptr || archive_entry_hardlink(entry) != nullptr) {
      if (error)
        *error = "archive contains link entry: " + entry_path;
      success = false;
      break;
    }

    if (entry_type != AE_IFREG && entry_type != AE_IFDIR) {
      if (error)
        *error = "archive contains unsupported entry type: " + entry_path;
      success = false;
      break;
    }

    std::error_code create_ec;
    fs::path check_path = full_dest;
    if (entry_type == AE_IFDIR) {
      fs::create_directories(full_dest, create_ec);
    } else {
      fs::create_directories(full_dest.parent_path(), create_ec);
      check_path = full_dest.parent_path();
    }
    if (create_ec) {
      if (error)
        *error = "failed to prepare extraction path: " + create_ec.message();
      success = false;
      break;
    }

    if (!vinput::path::IsPathWithinRoot(fs::weakly_canonical(check_path), dest_root)) {
      if (error)
        *error = "archive entry escapes extraction root: " + entry_path;
      success = false;
      break;
    }

    // Set the destination path
    archive_entry_set_pathname(entry, full_dest.c_str());

    r = archive_write_header(out, entry);
    if (r != ARCHIVE_OK) {
      if (error)
        *error = std::string("failed to write header: ") + archive_error_string(out);
      success = false;
      break;
    }

    if (archive_entry_size(entry) > 0) {
      // Copy data blocks
      const void* buff;
      size_t size;
      la_int64_t offset;
      while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
        if (archive_write_data_block(out, buff, size, offset) != ARCHIVE_OK) {
          if (error)
            *error = std::string("failed to write data: ") + archive_error_string(out);
          success = false;
          break;
        }
      }
      if (!success)
        break;
      if (r != ARCHIVE_EOF) {
        if (error)
          *error = std::string("archive read error: ") + archive_error_string(a);
        success = false;
        break;
      }
    }

    archive_write_finish_entry(out);
  }

  if (success && r != ARCHIVE_EOF) {
    if (error)
      *error = std::string("archive iteration error: ") + archive_error_string(a);
    success = false;
  }

  archive_read_close(a);
  archive_read_free(a);
  archive_write_close(out);
  archive_write_free(out);

  return success;
}
