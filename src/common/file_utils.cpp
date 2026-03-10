#include "common/file_utils.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fstream>

namespace vinput::file {

bool EnsureParentDirectory(const std::filesystem::path& path, std::string* error) {
  auto parent = path.parent_path();
  if (parent.empty() || std::filesystem::exists(parent)) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    if (error) *error = "Failed to create directory " + parent.string() + ": " + ec.message();
    return false;
  }
  return true;
}

bool AtomicWriteTextFile(const std::filesystem::path& target, std::string_view content, std::string* error) {
  auto tmp_path = target;
  tmp_path += ".tmp.XXXXXX";
  std::string tmp_str = tmp_path.string();

  int fd = mkstemp(tmp_str.data());
  if (fd < 0) {
    if (error) *error = std::string("mkstemp failed: ") + std::strerror(errno);
    return false;
  }

  // Write content
  const char* data = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    ssize_t written = write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      if (error) *error = std::string("write failed: ") + std::strerror(errno);
      close(fd);
      unlink(tmp_str.c_str());
      return false;
    }
    data += written;
    remaining -= static_cast<size_t>(written);
  }

  if (fsync(fd) != 0) {
    if (error) *error = std::string("fsync failed: ") + std::strerror(errno);
    close(fd);
    unlink(tmp_str.c_str());
    return false;
  }

  close(fd);

  std::error_code ec;
  std::filesystem::rename(tmp_str, target, ec);
  if (ec) {
    if (error) *error = "rename failed: " + ec.message();
    unlink(tmp_str.c_str());
    return false;
  }

  return true;
}

}  // namespace vinput::file
