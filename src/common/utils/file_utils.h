#pragma once
#include <cstddef>
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

bool ResolveSymlinkPath(const std::filesystem::path& path, std::filesystem::path* resolved,
                        std::string* error);
bool EnsureParentDirectory(const std::filesystem::path& path, std::string* error);
bool ReadTextFile(const std::filesystem::path& path, std::string* content,
                  std::string* error = nullptr, size_t max_bytes = 0);
bool AtomicWriteTextFile(const std::filesystem::path& target, std::string_view content,
                         std::string* error);
std::string ComputeFileSha256(const std::filesystem::path& path, std::string* error = nullptr);
} // namespace vinput::file
