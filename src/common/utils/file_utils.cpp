#include "common/utils/file_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <openssl/evp.h>
#include <openssl/types.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

extern "C" {
#include <stdlib.h>
}

namespace {

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
    if (error) {
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
    if (error) {
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
    if (error) {
      *error = "Failed to read symlink " + path.string() + ": " + ec.message();
    }
    return false;
  }

  return ResolveSymlinkPathImpl(ResolveRelativeSymlinkTarget(path, link_target), resolved, error,
                                depth + 1);
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

  char* result = ::mkdtemp(tmpl.data());
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

bool ResolveSymlinkPath(const std::filesystem::path& path, std::filesystem::path* resolved,
                        std::string* error) {
  if (!resolved) {
    if (error) {
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
    if (error)
      *error = "Failed to create directory " + parent.string() + ": " + ec.message();
    return false;
  }
  return true;
}

bool ReadTextFile(const std::filesystem::path& path, std::string* content, std::string* error,
                  size_t max_bytes) {
  if (content == nullptr) {
    if (error != nullptr) {
      *error = "ReadTextFile requires a destination buffer.";
    }
    return false;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error != nullptr) {
      *error = "Failed to open file " + path.string();
    }
    return false;
  }

  content->clear();
  if (max_bytes > 0) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    size_t initial_cap = 4096;
    if (!ec && sz > 0) {
      initial_cap = std::min(max_bytes, static_cast<size_t>(sz));
    }
    content->reserve(initial_cap);

    char chunk[8192];
    size_t total_read = 0;
    while (total_read < max_bytes) {
      const size_t to_read = std::min(sizeof(chunk), max_bytes - total_read);
      file.read(chunk, static_cast<std::streamsize>(to_read));
      const std::streamsize bytes_read = file.gcount();
      if (bytes_read <= 0) {
        break;
      }
      content->append(chunk, static_cast<size_t>(bytes_read));
      total_read += static_cast<size_t>(bytes_read);
    }
  } else {
    content->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  if (!file.good() && !file.eof()) {
    if (error != nullptr) {
      *error = "Failed to read file " + path.string();
    }
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool AtomicWriteTextFile(const std::filesystem::path& target, std::string_view content,
                         std::string* error) {
  std::filesystem::path resolved_target;
  if (!ResolveSymlinkPath(target, &resolved_target, error)) {
    return false;
  }

  if (!EnsureParentDirectory(resolved_target, error)) {
    return false;
  }

  auto tmp_path =
      resolved_target.parent_path() / (resolved_target.filename().string() + ".tmp.XXXXXX");
  std::string tmp_str = tmp_path.string();

  int fd = mkstemp(tmp_str.data());
  if (fd < 0) {
    if (error)
      *error = std::string("mkstemp failed: ") + std::strerror(errno);
    return false;
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
    ssize_t written = write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      if (error)
        *error = std::string("write failed: ") + std::strerror(errno);
      close(fd);
      unlink(tmp_str.c_str());
      return false;
    }
    data += written;
    remaining -= static_cast<size_t>(written);
  }

  if (fsync(fd) != 0) {
    if (error)
      *error = std::string("fsync failed: ") + std::strerror(errno);
    close(fd);
    unlink(tmp_str.c_str());
    return false;
  }

  close(fd);

  std::error_code ec;
  std::filesystem::rename(tmp_str, resolved_target, ec);
  if (ec) {
    if (error)
      *error = "rename failed: " + ec.message();
    unlink(tmp_str.c_str());
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::string ComputeFileSha256(const std::filesystem::path& path, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error != nullptr) {
      *error = "failed to open file for SHA-256 calculation: " + path.string();
    }
    return {};
  }

  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                    &EVP_MD_CTX_free);
  if (ctx == nullptr) {
    if (error != nullptr) {
      *error = "failed to allocate EVP_MD_CTX";
    }
    return {};
  }

  if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
    if (error != nullptr) {
      *error = "EVP_DigestInit_ex failed";
    }
    return {};
  }

  char buffer[8192];
  while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
    if (EVP_DigestUpdate(ctx.get(), buffer, static_cast<size_t>(file.gcount())) != 1) {
      if (error != nullptr) {
        *error = "EVP_DigestUpdate failed";
      }
      return {};
    }
  }

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len = 0;
  if (EVP_DigestFinal_ex(ctx.get(), hash, &hash_len) != 1) {
    if (error != nullptr) {
      *error = "EVP_DigestFinal_ex failed";
    }
    return {};
  }

  std::string hex;
  hex.reserve(static_cast<size_t>(hash_len) * 2);
  for (size_t i = 0; i < static_cast<size_t>(hash_len); ++i) {
    char pair[3];
    std::snprintf(pair, sizeof(pair), "%02x", *(hash + i));
    hex += pair;
  }

  if (error != nullptr) {
    error->clear();
  }
  return hex;
}

} // namespace vinput::file
