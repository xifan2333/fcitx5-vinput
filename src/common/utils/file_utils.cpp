#include "common/utils/file_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <mio/mmap.hpp>
#include <span>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "common/utils/expected.h"

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

bool FailSymlinkResolution(std::string* error, std::string_view message) {
  if (error != nullptr) {
    *error = std::string(message);
  }
  return false;
}

bool ResolveSymlinkPathImpl(const std::filesystem::path& path, std::filesystem::path* resolved,
                            std::string* error, int depth);

Expected<std::filesystem::path> ResolveMissingPathParent(const std::filesystem::path& path,
                                                         int depth) {
  const auto parent = path.parent_path();
  if (parent.empty()) {
    return path;
  }

  std::filesystem::path resolved_parent;
  std::string err;
  if (!ResolveSymlinkPathImpl(parent, &resolved_parent, &err, depth)) {
    return UnexpectedError(err);
  }

  const auto filename = path.filename();
  if (filename.empty()) {
    return resolved_parent;
  }
  return (resolved_parent / filename).lexically_normal();
}

bool ResolveSymlinkPathImpl(const std::filesystem::path& path, std::filesystem::path* resolved,
                            std::string* error, int depth) {
  if (depth >= kMaxSymlinkDepth) {
    return FailSymlinkResolution(error, "Too many symlink levels while resolving " + path.string());
  }

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) {
    auto missing = ResolveMissingPathParent(path, depth);
    if (!missing) {
      return FailSymlinkResolution(error, missing.error());
    }
    *resolved = std::move(*missing);
    return true;
  }
  if (ec) {
    return FailSymlinkResolution(error,
                                 "Failed to inspect path " + path.string() + ": " + ec.message());
  }
  if (status.type() != std::filesystem::file_type::symlink) {
    *resolved = path;
    return true;
  }

  const auto link_target = std::filesystem::read_symlink(path, ec);
  if (ec) {
    return FailSymlinkResolution(error,
                                 "Failed to read symlink " + path.string() + ": " + ec.message());
  }

  return ResolveSymlinkPathImpl(ResolveRelativeSymlinkTarget(path, link_target), resolved, error,
                                depth + 1);
}

constexpr size_t kStreamBufferSize = 65536;
constexpr std::string_view kEmptyFileSha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

/// Self-contained pure C++20 SHA-256 implementation.
/// Eliminates the OpenSSL dependency for file hashing while preserving exact
/// byte-for-byte hash output with the previous EVP_sha256 backend.
class Sha256Context {
public:
  void Update(std::string_view data) {
    const char* bytes = data.data();
    size_t len = data.size();
    total_len_ += len;

    if (buffer_len_ > 0) {
      const size_t needed = 64 - buffer_len_;
      const size_t take = std::min(needed, len);
      std::memcpy(buffer_.data() + buffer_len_, bytes, take);
      buffer_len_ += take;
      bytes += take;
      len -= take;

      if (buffer_len_ == 64) {
        ProcessBlock(buffer_);
        buffer_len_ = 0;
      }
    }

    while (len >= 64) {
      ProcessBlock(std::span<const char, 64>(bytes, 64));
      bytes += 64;
      len -= 64;
    }

    if (len > 0) {
      std::memcpy(buffer_.data(), bytes, len);
      buffer_len_ = len;
    }
  }

  std::array<unsigned char, 32> Final() {
    total_len_ *= 8;

    buffer_.at(buffer_len_++) = static_cast<char>(0x80);
    if (buffer_len_ > 56) {
      while (buffer_len_ < 64) {
        buffer_.at(buffer_len_++) = 0;
      }
      ProcessBlock(buffer_);
      buffer_len_ = 0;
    }

    while (buffer_len_ < 56) {
      buffer_.at(buffer_len_++) = 0;
    }

    for (int i = 7; i >= 0; --i) {
      buffer_.at(buffer_len_++) = static_cast<char>((total_len_ >> (i * 8)) & 0xFF);
    }
    ProcessBlock(buffer_);

    std::array<unsigned char, 32> digest{};
    for (size_t i = 0; i < 8; ++i) {
      const std::uint32_t word = state_.at(i);
      digest.at(i * 4) = static_cast<unsigned char>((word >> 24) & 0xFF);
      digest.at((i * 4) + 1) = static_cast<unsigned char>((word >> 16) & 0xFF);
      digest.at((i * 4) + 2) = static_cast<unsigned char>((word >> 8) & 0xFF);
      digest.at((i * 4) + 3) = static_cast<unsigned char>(word & 0xFF);
    }
    return digest;
  }

private:
  static constexpr std::array<std::uint32_t, 64> kRoundConstants = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};

  static constexpr std::uint32_t Rotr(std::uint32_t x, std::uint32_t n) {
    return (x >> n) | (x << (32 - n));
  }

  static constexpr std::uint32_t ByteAt(std::span<const char, 64> block, size_t idx) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(block[idx]));
  }

  void ProcessBlock(std::span<const char, 64> block) {
    std::array<std::uint32_t, 64> w{};
    for (size_t i = 0; i < 16; ++i) {
      w.at(i) = (ByteAt(block, i * 4) << 24) | (ByteAt(block, (i * 4) + 1) << 16) |
                (ByteAt(block, (i * 4) + 2) << 8) | ByteAt(block, (i * 4) + 3);
    }
    for (size_t i = 16; i < 64; ++i) {
      const std::uint32_t w15 = w.at(i - 15);
      const std::uint32_t w2 = w.at(i - 2);
      const std::uint32_t s0 = Rotr(w15, 7) ^ Rotr(w15, 18) ^ (w15 >> 3);
      const std::uint32_t s1 = Rotr(w2, 17) ^ Rotr(w2, 19) ^ (w2 >> 10);
      w.at(i) = w.at(i - 16) + s0 + w.at(i - 7) + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (size_t i = 0; i < 64; ++i) {
      const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + s1 + ch + kRoundConstants.at(i) + w.at(i);
      const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<char, 64> buffer_{};
  size_t buffer_len_ = 0;
  std::uint64_t total_len_ = 0;
};

std::string DigestToHex(const std::array<unsigned char, 32>& digest) {
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const unsigned char byte : digest) {
    fmt::format_to(std::back_inserter(hex), "{:02x}", byte);
  }
  return hex;
}

Expected<std::string> ComputeFileSha256StreamFallback(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return UnexpectedError("failed to open file for SHA-256 calculation: " + path.string());
  }

  Sha256Context ctx;
  char buffer[kStreamBufferSize];
  while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
    ctx.Update(std::string_view(buffer, static_cast<size_t>(file.gcount())));
  }

  return DigestToHex(ctx.Final());
}

Expected<std::filesystem::path> ResolveTempBaseDir(const std::filesystem::path& base_dir) {
  std::error_code ec;
  if (base_dir.empty()) {
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
      return UnexpectedError("Failed to determine system temporary directory: " + ec.message());
    }
    return dir;
  }
  std::filesystem::create_directories(base_dir, ec);
  if (ec) {
    return UnexpectedError("Failed to create directory " + base_dir.string() + ": " + ec.message());
  }
  return base_dir;
}

std::string MakeMkdtempTemplate(const std::filesystem::path& dir, std::string_view prefix) {
  if (prefix.ends_with("XXXXXX")) {
    return (dir / prefix).string();
  }
  return (dir / (std::string(prefix) + "XXXXXX")).string();
}

Expected<std::string> ComputeMappingSha256(const vinput::file::ReadOnlyFileMapping& mapping) {
  Sha256Context ctx;
  const auto sv = mapping.StringView();
  if (!sv.empty()) {
    ctx.Update(sv);
  }
  return DigestToHex(ctx.Final());
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

std::unique_ptr<ScopedTempDir> ScopedTempDir::Create(const std::filesystem::path& base_dir,
                                                     std::string_view prefix, std::string* error) {
  const auto dir = ResolveTempBaseDir(base_dir);
  if (!dir) {
    if (error != nullptr) {
      *error = dir.error();
    }
    return nullptr;
  }

  std::string tmpl = MakeMkdtempTemplate(*dir, prefix);
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
  if (fs_ec || !std::filesystem::exists(status) || std::filesystem::is_directory(status)) {
    return UnexpectedError("failed to open file for SHA-256 calculation: " + path.string());
  }

  const auto file_size = std::filesystem::file_size(path, fs_ec);
  if (!fs_ec && file_size == 0) {
    std::ifstream probe(path, std::ios::binary);
    if (!probe.is_open()) {
      return UnexpectedError("failed to open file for SHA-256 calculation: " + path.string());
    }
    if (probe.peek() == std::ifstream::traits_type::eof()) {
      return std::string(kEmptyFileSha256);
    }
    return ComputeFileSha256StreamFallback(path);
  }

  if (auto mapping = ReadOnlyFileMapping::Map(path)) {
    return ComputeMappingSha256(*mapping);
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
    return fmt::format("{:.1f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
  }
  if (bytes >= 1024ULL * 1024) {
    return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024));
  }
  if (bytes >= 1024) {
    return fmt::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
  }
  return fmt::format("{} B", bytes);
}

} // namespace vinput::file
