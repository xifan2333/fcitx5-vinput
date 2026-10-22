#include "common/utils/file_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <mio/mmap.hpp>
#include <openssl/evp.h>
#include <openssl/types.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "common/utils/expected.h"

extern "C" {
#include <stdlib.h>
}

namespace {

using vinput::Expected;
using vinput::UnexpectedError;

constexpr int kMaxSymlinkDepth = 32;

std::filesystem::path ResolveRelativeSymlinkTarget(const std::filesystem::path& symlink_path,
                                                   const std::filesystem::path& link_target) {
  if (link_target.is_absolute()) {
    return link_target;
  }

  const auto parent = symlink_path.parent_path();
  if (parent.empty()) {
    return link_target.lexically_normal();
  }

  std::error_code ec;
  const auto canonical_parent = std::filesystem::canonical(parent, ec);
  if (!ec) {
    return (canonical_parent / link_target).lexically_normal();
  }
  return (parent / link_target).lexically_normal();
}

bool ResolveSymlinkPathImpl(const std::filesystem::path& path, std::filesystem::path* resolved,
                            std::string* error, int depth) {
  if (depth >= kMaxSymlinkDepth) {
    if (error != nullptr) {
      *error = "Too many symlink levels while resolving " + path.string();
    }
    return false;
  }

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
      *resolved = path;
      return true;
    }

    std::filesystem::path resolved_parent;
    if (!ResolveSymlinkPathImpl(parent, &resolved_parent, error, depth)) {
      return false;
    }

    const auto filename = path.filename();
    if (filename.empty()) {
      *resolved = resolved_parent;
    } else {
      *resolved = (resolved_parent / filename).lexically_normal();
    }
    return true;
  }
  if (ec) {
    if (error != nullptr) {
      *error = "Failed to inspect path " + path.string() + ": " + ec.message();
    }
    return false;
  }
  if (status.type() != std::filesystem::file_type::symlink) {
    *resolved = path;
    return true;
  }

  const auto link_target = std::filesystem::read_symlink(path, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "Failed to read symlink " + path.string() + ": " + ec.message();
    }
    return false;
  }

  return ResolveSymlinkPathImpl(ResolveRelativeSymlinkTarget(path, link_target), resolved, error,
                                depth + 1);
}

constexpr size_t kStreamBufferSize = 65536;
constexpr std::string_view kEmptyFileSha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

Expected<std::string> FinalizeHashToHex(EVP_MD_CTX* ctx) {
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len = 0;
  if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
    return UnexpectedError("EVP_DigestFinal_ex failed");
  }

  std::string hex;
  hex.reserve(static_cast<size_t>(hash_len) * 2);
  for (size_t i = 0; i < static_cast<size_t>(hash_len); ++i) {
    char pair[3];
    std::snprintf(pair, sizeof(pair), "%02x", *(hash + i));
    hex += pair;
  }

  return hex;
}

Expected<std::string> ComputeFileSha256StreamFallback(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return UnexpectedError("failed to open file for SHA-256 calculation: " + path.string());
  }

  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                    &EVP_MD_CTX_free);
  if (ctx == nullptr) {
    return UnexpectedError("failed to allocate EVP_MD_CTX");
  }

  if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
    return UnexpectedError("EVP_DigestInit_ex failed");
  }

  char buffer[kStreamBufferSize];
  while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
    if (EVP_DigestUpdate(ctx.get(), buffer, static_cast<size_t>(file.gcount())) != 1) {
      return UnexpectedError("EVP_DigestUpdate failed");
    }
  }

  return FinalizeHashToHex(ctx.get());
}

} // namespace

namespace vinput::file {

ScopedTempDir::ScopedTempDir(std::filesystem::path path)
    : path_(std::move(path)), active_(!path_.empty()) {}

ScopedTempDir::~ScopedTempDir() {
  Remove();
}

ScopedTempDir::ScopedTempDir(ScopedTempDir&& other) noexcept
    : path_(std::move(other.path_)), active_(other.active_) {
  other.active_ = false;
  other.path_.clear();
}

ScopedTempDir& ScopedTempDir::operator=(ScopedTempDir&& other) noexcept {
  if (this != &other) {
    Remove();
    path_ = std::move(other.path_);
    active_ = other.active_;
    other.active_ = false;
    other.path_.clear();
  }
  return *this;
}

std::unique_ptr<ScopedTempDir> ScopedTempDir::Create(const std::filesystem::path& base_dir,
                                                     std::string_view prefix, std::string* error) {
  std::error_code ec;
  std::filesystem::path dir = base_dir;
  if (dir.empty()) {
    dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
      if (error != nullptr) {
        *error = "Failed to determine system temporary directory: " + ec.message();
      }
      return nullptr;
    }
  } else {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "Failed to create directory " + dir.string() + ": " + ec.message();
      }
      return nullptr;
    }
  }

  std::string tmpl;
  if (prefix.ends_with("XXXXXX")) {
    tmpl = (dir / prefix).string();
  } else {
    tmpl = (dir / (std::string(prefix) + "XXXXXX")).string();
  }

  const char* result = ::mkdtemp(tmpl.data());
  if (result == nullptr) {
    if (error != nullptr) {
      *error = "Failed to create temporary directory: " + std::string(std::strerror(errno));
    }
    return nullptr;
  }

  return std::make_unique<ScopedTempDir>(std::filesystem::path(result));
}

void ScopedTempDir::Remove() {
  if (active_ && !path_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    active_ = false;
  }
}

void ScopedTempDir::Dismiss() {
  active_ = false;
}

const std::filesystem::path& ScopedTempDir::Path() const {
  return path_;
}

bool ScopedTempDir::IsValid() const {
  return active_ && !path_.empty();
}

struct ReadOnlyFileMapping::Impl {
  mio::mmap_source mmap;
};

ReadOnlyFileMapping::ReadOnlyFileMapping() = default;
ReadOnlyFileMapping::~ReadOnlyFileMapping() = default;
ReadOnlyFileMapping::ReadOnlyFileMapping(ReadOnlyFileMapping&& other) noexcept
    : impl_(std::move(other.impl_)), is_empty_file_(std::exchange(other.is_empty_file_, false)) {}

ReadOnlyFileMapping& ReadOnlyFileMapping::operator=(ReadOnlyFileMapping&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
    is_empty_file_ = std::exchange(other.is_empty_file_, false);
  }
  return *this;
}

Expected<ReadOnlyFileMapping> ReadOnlyFileMapping::Map(const std::filesystem::path& path) {
  std::error_code fs_ec;
  const auto status = std::filesystem::status(path, fs_ec);
  if (fs_ec || !std::filesystem::exists(status)) {
    return UnexpectedError("file does not exist or is inaccessible: " + path.string());
  }

  if (std::filesystem::is_directory(status)) {
    return UnexpectedError("cannot map directory: " + path.string());
  }

  ReadOnlyFileMapping result;
  const auto file_size = std::filesystem::file_size(path, fs_ec);
  if (!fs_ec && file_size == 0) {
    std::ifstream probe(path, std::ios::binary);
    if (!probe.is_open()) {
      return UnexpectedError("failed to open empty file for reading: " + path.string());
    }
    // Verify that the file is genuinely empty rather than a pseudo-file with size 0 (e.g.
    // /proc/...).
    if (probe.peek() == std::ifstream::traits_type::eof()) {
      result.is_empty_file_ = true;
      return result;
    }
    return UnexpectedError("cannot memory map pseudo-file with dynamic contents: " + path.string());
  }

  std::error_code mmap_ec;
  auto impl = std::make_unique<Impl>();
  impl->mmap = mio::make_mmap_source(path.string(), mmap_ec);
  if (mmap_ec || !impl->mmap.is_open()) {
    return UnexpectedError("failed to map file: " + mmap_ec.message());
  }

  result.impl_ = std::move(impl);
  return result;
}

bool ReadOnlyFileMapping::IsValid() const noexcept {
  return is_empty_file_ || (impl_ != nullptr && impl_->mmap.is_open());
}

std::string_view ReadOnlyFileMapping::StringView() const noexcept {
  if (is_empty_file_ || impl_ == nullptr || !impl_->mmap.is_open()) {
    return {};
  }
  return {impl_->mmap.data(), impl_->mmap.size()};
}

std::string_view ReadOnlyFileMapping::View() const noexcept {
  return StringView();
}

const char* ReadOnlyFileMapping::Data() const noexcept {
  if (is_empty_file_) {
    return "";
  }
  if (impl_ == nullptr || !impl_->mmap.is_open()) {
    return nullptr;
  }
  return impl_->mmap.data();
}

size_t ReadOnlyFileMapping::Size() const noexcept {
  if (is_empty_file_ || impl_ == nullptr || !impl_->mmap.is_open()) {
    return 0;
  }
  return impl_->mmap.size();
}

bool ReadOnlyFileMapping::IsEmpty() const noexcept {
  return Size() == 0;
}

Expected<ReadOnlyFileMapping> MapFileReadOnly(const std::filesystem::path& path) {
  return ReadOnlyFileMapping::Map(path);
}

bool ResolveSymlinkPath(const std::filesystem::path& path, std::filesystem::path* resolved,
                        std::string* error) {
  if (resolved == nullptr) {
    if (error != nullptr) {
      *error = "ResolveSymlinkPath requires a destination path.";
    }
    return false;
  }
  return ResolveSymlinkPathImpl(path, resolved, error, 0);
}

bool EnsureParentDirectory(const std::filesystem::path& path, std::string* error) {
  auto parent = path.parent_path();
  if (parent.empty() || std::filesystem::exists(parent)) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "Failed to create directory " + parent.string() + ": " + ec.message();
    }
    return false;
  }
  return true;
}

Expected<std::string> ReadTextFile(const std::filesystem::path& path, size_t max_bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return UnexpectedError("Failed to open file " + path.string());
  }

  std::string content;
  if (max_bytes > 0) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    size_t initial_cap = 4096;
    if (!ec && sz > 0) {
      initial_cap = std::min(max_bytes, static_cast<size_t>(sz));
    }
    content.reserve(initial_cap);

    char chunk[8192];
    size_t total_read = 0;
    while (total_read < max_bytes) {
      const size_t to_read = std::min(sizeof(chunk), max_bytes - total_read);
      file.read(chunk, static_cast<std::streamsize>(to_read));
      const std::streamsize bytes_read = file.gcount();
      if (bytes_read <= 0) {
        break;
      }
      content.append(chunk, static_cast<size_t>(bytes_read));
      total_read += static_cast<size_t>(bytes_read);
    }
  } else {
    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  if (!file.good() && !file.eof()) {
    return UnexpectedError("Failed to read file " + path.string());
  }

  return content;
}

Expected<void> AtomicWriteTextFile(const std::filesystem::path& target, std::string_view content) {
  std::filesystem::path resolved_target;
  std::string error;
  if (!ResolveSymlinkPath(target, &resolved_target, &error)) {
    return UnexpectedError(error);
  }

  if (!EnsureParentDirectory(resolved_target, &error)) {
    return UnexpectedError(error);
  }

  auto tmp_path =
      resolved_target.parent_path() / (resolved_target.filename().string() + ".tmp.XXXXXX");
  std::string tmp_str = tmp_path.string();

  const int fd = mkstemp(tmp_str.data());
  if (fd < 0) {
    return UnexpectedError(std::string("mkstemp failed: ") + std::strerror(errno));
  }

  std::error_code status_ec;
  const auto target_status = std::filesystem::status(resolved_target, status_ec);
  if (!status_ec && std::filesystem::exists(target_status)) {
    ::fchmod(fd, static_cast<mode_t>(target_status.permissions() & std::filesystem::perms::all));
  }

  // Write content
  const char* data = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    const ssize_t written = write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string err = std::string("write failed: ") + std::strerror(errno);
      close(fd);
      unlink(tmp_str.c_str());
      return UnexpectedError(err);
    }
    data += written;
    remaining -= static_cast<size_t>(written);
  }

  if (fsync(fd) != 0) {
    const std::string err = std::string("fsync failed: ") + std::strerror(errno);
    close(fd);
    unlink(tmp_str.c_str());
    return UnexpectedError(err);
  }

  close(fd);

  std::error_code ec;
  std::filesystem::rename(tmp_str, resolved_target, ec);
  if (ec) {
    unlink(tmp_str.c_str());
    return UnexpectedError("rename failed: " + ec.message());
  }

  return {};
}

Expected<std::string> ComputeFileSha256(const std::filesystem::path& path) {
  std::error_code fs_ec;
  const auto status = std::filesystem::status(path, fs_ec);
  if (fs_ec || !std::filesystem::exists(status)) {
    return UnexpectedError("failed to open file for SHA-256 calculation: " + path.string());
  }

  if (std::filesystem::is_directory(status)) {
    return UnexpectedError("failed to open file for SHA-256 calculation: " + path.string());
  }

  const auto file_size = std::filesystem::file_size(path, fs_ec);
  if (!fs_ec && file_size == 0) {
    // Defense in depth: POSIX mmap returns EINVAL on 0-byte mappings.
    // Verify file readability first.
    std::ifstream probe(path, std::ios::binary);
    if (!probe.is_open()) {
      return UnexpectedError("failed to open file for SHA-256 calculation: " + path.string());
    }
    // Confirm it's truly empty, rather than a virtual pseudo-file with st_size = 0 (e.g. /proc/...)
    if (probe.peek() == std::ifstream::traits_type::eof()) {
      return std::string(kEmptyFileSha256);
    }
    // If it yields bytes despite st_size == 0, fallback to streaming read.
    return ComputeFileSha256StreamFallback(path);
  }

  auto mapping = ReadOnlyFileMapping::Map(path);
  if (mapping) {
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                      &EVP_MD_CTX_free);
    if (ctx == nullptr) {
      return UnexpectedError("failed to allocate EVP_MD_CTX");
    }

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
      return UnexpectedError("EVP_DigestInit_ex failed");
    }

    const auto sv = mapping->StringView();
    if (!sv.empty()) {
      if (EVP_DigestUpdate(ctx.get(), sv.data(), sv.size()) != 1) {
        return UnexpectedError("EVP_DigestUpdate failed");
      }
    }

    return FinalizeHashToHex(ctx.get());
  }

  return ComputeFileSha256StreamFallback(path);
}

bool IsDirectoryWritable(const std::filesystem::path& path) {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && !ec) {
    if (std::filesystem::is_directory(path, ec) && !ec) {
      return ::access(path.c_str(), W_OK | X_OK) == 0;
    }
    return false;
  }
  const auto parent = path.parent_path();
  if (!parent.empty() && parent != path) {
    if (std::filesystem::exists(parent, ec) && !ec) {
      if (std::filesystem::is_directory(parent, ec) && !ec) {
        return ::access(parent.c_str(), W_OK | X_OK) == 0;
      }
    }
  }
  return false;
}

std::string FormatFileSize(uint64_t bytes) {
  if (bytes >= 1024ULL * 1024 * 1024) {
    return std::format("{:.1f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
  }
  if (bytes >= 1024ULL * 1024) {
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024));
  }
  if (bytes >= 1024) {
    return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
  }
  return std::format("{} B", bytes);
}

} // namespace vinput::file
