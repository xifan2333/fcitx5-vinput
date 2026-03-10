#include "common/model_repository.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <openssl/evp.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/file_utils.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

struct WriteState {
  std::ofstream *out = nullptr;
};

size_t CurlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *state = static_cast<WriteState *>(userdata);
  const size_t total = size * nmemb;
  state->out->write(ptr, static_cast<std::streamsize>(total));
  if (!state->out->good()) return 0;
  return total;
}

struct MemoryBuffer {
  std::string data;
};

size_t CurlMemoryWriteCallback(char *ptr, size_t size, size_t nmemb,
                               void *userdata) {
  auto *buf = static_cast<MemoryBuffer *>(userdata);
  const size_t total = size * nmemb;
  buf->data.append(ptr, total);
  return total;
}

struct ProgressState {
  ProgressCallback cb;
  uint64_t total_bytes = 0;
};

int CurlXferInfoCallback(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  auto *state = static_cast<ProgressState *>(userdata);
  if (state->cb) {
    InstallProgress p;
    p.downloaded_bytes = static_cast<uint64_t>(dlnow);
    p.total_bytes = static_cast<uint64_t>(dltotal > 0 ? dltotal : state->total_bytes);
    state->cb(p);
  }
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// ModelRepository
// ---------------------------------------------------------------------------

ModelRepository::ModelRepository(const std::string &base_dir)
    : base_dir_(base_dir) {}

std::vector<RemoteModelEntry>
ModelRepository::FetchRegistry(const std::string &registry_url,
                               std::string *error) const {
  std::vector<RemoteModelEntry> entries;

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (error) *error = "failed to initialize libcurl";
    return entries;
  }

  MemoryBuffer buf;
  curl_easy_setopt(curl, CURLOPT_URL, registry_url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlMemoryWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    if (error)
      *error = std::string("curl error: ") + curl_easy_strerror(res);
    return entries;
  }
  if (http_code != 200) {
    if (error)
      *error = "registry fetch failed with HTTP " + std::to_string(http_code);
    return entries;
  }

  try {
    json j = json::parse(buf.data);
    if (!j.is_array()) {
      if (error) *error = "registry JSON is not an array";
      return entries;
    }
    for (const auto &item : j) {
      RemoteModelEntry e;
      e.name = item.value("name", "");
      e.display_name = item.value("display_name", "");
      e.description = item.value("description", "");
      e.url = item.value("url", "");
      e.sha256 = item.value("sha256", "");
      e.size_bytes = item.value("size_bytes", uint64_t{0});
      e.model_type = item.value("model_type", "");
      e.language = item.value("language", "");
      if (item.contains("vinput_model")) {
        e.vinput_model = item["vinput_model"];
      }
      if (!e.name.empty() && !e.url.empty()) {
        entries.push_back(std::move(e));
      }
    }
  } catch (const std::exception &ex) {
    if (error)
      *error = std::string("failed to parse registry JSON: ") + ex.what();
    return entries;
  }

  return entries;
}

bool ModelRepository::InstallModel(const std::string &registry_url,
                                   const std::string &model_name,
                                   ProgressCallback progress_cb,
                                   std::string *error) const {
  // Fetch registry
  std::string fetch_err;
  auto entries = FetchRegistry(registry_url, &fetch_err);
  if (!fetch_err.empty()) {
    if (error) *error = fetch_err;
    return false;
  }

  // Find the requested model
  const RemoteModelEntry *found = nullptr;
  for (const auto &e : entries) {
    if (e.name == model_name) {
      found = &e;
      break;
    }
  }
  if (!found) {
    if (error) *error = "model not found in registry: " + model_name;
    return false;
  }

  // Create temporary directory
  const fs::path tmp_dir = base_dir_ / fs::path(".tmp-" + model_name);
  std::error_code ec;
  fs::remove_all(tmp_dir, ec); // clean up any leftover
  if (!fs::create_directories(tmp_dir, ec)) {
    if (error) *error = "failed to create temp dir: " + tmp_dir.string();
    return false;
  }

  // Determine archive filename from URL
  std::string url_path = found->url;
  std::string archive_name = model_name + ".tar.gz";
  {
    auto pos = url_path.rfind('/');
    if (pos != std::string::npos && pos + 1 < url_path.size()) {
      archive_name = url_path.substr(pos + 1);
      // Strip query string if present
      auto q = archive_name.find('?');
      if (q != std::string::npos) archive_name = archive_name.substr(0, q);
    }
  }

  const fs::path archive_path = tmp_dir / archive_name;

  // Download
  std::string dl_err;
  if (!DownloadFile(found->url, archive_path, progress_cb, &dl_err)) {
    fs::remove_all(tmp_dir, ec);
    if (error) *error = dl_err;
    return false;
  }

  // Verify SHA256 (stub)
  std::string verify_err;
  if (!VerifySha256(archive_path, found->sha256, &verify_err)) {
    fs::remove_all(tmp_dir, ec);
    if (error) *error = verify_err;
    return false;
  }

  // Extract
  const fs::path extracted_dir = tmp_dir / "extracted";
  fs::create_directories(extracted_dir, ec);
  std::string extract_err;
  if (!ExtractArchive(archive_path, extracted_dir, &extract_err)) {
    fs::remove_all(tmp_dir, ec);
    if (error) *error = extract_err;
    return false;
  }

  // Write vinput-model.json if provided
  if (!found->vinput_model.is_null() && !found->vinput_model.empty()) {
    const fs::path json_path = extracted_dir / "vinput-model.json";
    std::ofstream jf(json_path);
    if (!jf.is_open()) {
      fs::remove_all(tmp_dir, ec);
      if (error) *error = "failed to write vinput-model.json";
      return false;
    }
    jf << found->vinput_model.dump(2);
    jf.close();
  }

  // Atomic rename to final destination
  const fs::path dest_dir = base_dir_ / model_name;
  fs::remove_all(dest_dir, ec); // remove existing if any
  fs::rename(extracted_dir, dest_dir, ec);
  if (ec) {
    // rename may fail across filesystems; try copy + remove
    std::error_code copy_ec;
    fs::copy(extracted_dir, dest_dir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             copy_ec);
    if (copy_ec) {
      fs::remove_all(tmp_dir, ec);
      if (error) *error = "failed to install model: " + copy_ec.message();
      return false;
    }
  }

  // Clean up temp dir
  fs::remove_all(tmp_dir, ec);
  return true;
}

bool ModelRepository::DownloadFile(const std::string &url,
                                   const fs::path &dest, ProgressCallback cb,
                                   std::string *error) const {
  std::string err_msg;
  if (!vinput::file::EnsureParentDirectory(dest, &err_msg)) {
    if (error) *error = err_msg;
    return false;
  }

  std::ofstream out(dest, std::ios::binary);
  if (!out.is_open()) {
    if (error) *error = "failed to open destination file: " + dest.string();
    return false;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (error) *error = "failed to initialize libcurl";
    return false;
  }

  WriteState ws{&out};
  ProgressState ps{cb, 0};

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ws);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlXferInfoCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ps);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  out.close();

  if (res != CURLE_OK) {
    std::error_code ec;
    fs::remove(dest, ec);
    if (error)
      *error = std::string("download failed: ") + curl_easy_strerror(res);
    return false;
  }
  if (http_code != 200) {
    std::error_code ec;
    fs::remove(dest, ec);
    if (error)
      *error = "download failed with HTTP " + std::to_string(http_code);
    return false;
  }

  return true;
}

bool ModelRepository::VerifySha256(const fs::path &file,
                                   const std::string &expected,
                                   std::string *error) const {
  if (expected.empty()) return true;

  std::ifstream in(file, std::ios::binary);
  if (!in.is_open()) {
    if (error) *error = "failed to open file for SHA256 verification: " + file.string();
    return false;
  }

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    if (error) *error = "failed to create EVP_MD_CTX";
    return false;
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    if (error) *error = "EVP_DigestInit_ex failed";
    return false;
  }

  char buf[8192];
  while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
    if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(in.gcount())) != 1) {
      EVP_MD_CTX_free(ctx);
      if (error) *error = "EVP_DigestUpdate failed";
      return false;
    }
  }

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len = 0;
  if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
    EVP_MD_CTX_free(ctx);
    if (error) *error = "EVP_DigestFinal_ex failed";
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

bool ModelRepository::ExtractArchive(const fs::path &archive,
                                     const fs::path &dest,
                                     std::string *error) const {
  struct archive *a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);

  struct archive *out = archive_write_disk_new();
  archive_write_disk_set_options(
      out, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL |
               ARCHIVE_EXTRACT_FFLAGS);
  archive_write_disk_set_standard_lookup(out);

  int r = archive_read_open_filename(a, archive.c_str(), 16384);
  if (r != ARCHIVE_OK) {
    if (error)
      *error = std::string("failed to open archive: ") +
               archive_error_string(a);
    archive_read_free(a);
    archive_write_free(out);
    return false;
  }

  bool success = true;
  struct archive_entry *entry;
  while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    const char *raw_path = archive_entry_pathname(entry);
    if (!raw_path) {
      continue;
    }

    std::string entry_path(raw_path);

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
      for (const auto &component : check_path) {
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

    // Set the destination path
    const fs::path full_dest = dest / entry_path;
    archive_entry_set_pathname(entry, full_dest.c_str());

    r = archive_write_header(out, entry);
    if (r != ARCHIVE_OK) {
      if (error)
        *error = std::string("failed to write header: ") +
                 archive_error_string(out);
      success = false;
      break;
    }

    if (archive_entry_size(entry) > 0) {
      // Copy data blocks
      const void *buff;
      size_t size;
      la_int64_t offset;
      while ((r = archive_read_data_block(a, &buff, &size, &offset)) ==
             ARCHIVE_OK) {
        if (archive_write_data_block(out, buff, size, offset) != ARCHIVE_OK) {
          if (error)
            *error = std::string("failed to write data: ") +
                     archive_error_string(out);
          success = false;
          break;
        }
      }
      if (!success) break;
      if (r != ARCHIVE_EOF) {
        if (error)
          *error = std::string("archive read error: ") +
                   archive_error_string(a);
        success = false;
        break;
      }
    }

    archive_write_finish_entry(out);
  }

  if (success && r != ARCHIVE_EOF) {
    if (error)
      *error =
          std::string("archive iteration error: ") + archive_error_string(a);
    success = false;
  }

  archive_read_close(a);
  archive_read_free(a);
  archive_write_close(out);
  archive_write_free(out);

  return success;
}
