#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace vinput::file {

class ScopedTempDir {
public:
  ScopedTempDir() = default;
  explicit ScopedTempDir(std::filesystem::path path);
  ~ScopedTempDir();

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  ScopedTempDir(ScopedTempDir&& other) noexcept;
  ScopedTempDir& operator=(ScopedTempDir&& other) noexcept;

  static std::unique_ptr<ScopedTempDir> Create(const std::filesystem::path& base_dir = {},
                                               std::string_view prefix = ".tmp-",
                                               std::string* error = nullptr);

  [[nodiscard]] const std::filesystem::path& Path() const;
  [[nodiscard]] bool IsValid() const;
  void Dismiss();
  void Remove();

private:
  std::filesystem::path path_;
  bool active_{false};
};

// RAII wrapper for zero-copy read-only memory-mapped file access.
// Expects the target file to remain immutable during the lifetime of the mapping.
// Safely handles 0-byte empty files by providing an empty string_view.
class ReadOnlyFileMapping {
public:
  ReadOnlyFileMapping();
  ~ReadOnlyFileMapping();

  ReadOnlyFileMapping(const ReadOnlyFileMapping&) = delete;
  ReadOnlyFileMapping& operator=(const ReadOnlyFileMapping&) = delete;

  ReadOnlyFileMapping(ReadOnlyFileMapping&& other) noexcept;
  ReadOnlyFileMapping& operator=(ReadOnlyFileMapping&& other) noexcept;

  // Maps the entire file into memory for read-only access.
  // For empty (0-byte) files, this succeeds and produces an empty StringView().
  static ReadOnlyFileMapping Map(const std::filesystem::path& path, std::string* error = nullptr);

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] std::string_view StringView() const noexcept;
  [[nodiscard]] std::string_view View() const noexcept;
  [[nodiscard]] const char* Data() const noexcept;
  [[nodiscard]] size_t Size() const noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool is_empty_file_{false};
};

// Convenience wrapper for ReadOnlyFileMapping::Map.
ReadOnlyFileMapping MapFileReadOnly(const std::filesystem::path& path,
                                    std::string* error = nullptr);

bool ResolveSymlinkPath(const std::filesystem::path& path, std::filesystem::path* resolved,
                        std::string* error);
bool EnsureParentDirectory(const std::filesystem::path& path, std::string* error);
bool ReadTextFile(const std::filesystem::path& path, std::string* content,
                  std::string* error = nullptr, size_t max_bytes = 0);
bool AtomicWriteTextFile(const std::filesystem::path& target, std::string_view content,
                         std::string* error);
// Computes the SHA-256 hash of a file.
// Uses zero-copy memory mapping for performance, falling back to streaming when mmap is
// unavailable or for pseudo-files. Expects the target file to remain immutable during hashing.
std::string ComputeFileSha256(const std::filesystem::path& path, std::string* error = nullptr);

// Check if a directory (or its immediate parent if target does not yet exist)
// exists, is a valid directory, and has write and search permissions (W_OK | X_OK).
[[nodiscard]] bool IsDirectoryWritable(const std::filesystem::path& path);

// Formats a file or transfer size in bytes to a human-readable string (B, KB, MB, GB).
std::string FormatFileSize(uint64_t bytes);
} // namespace vinput::file
