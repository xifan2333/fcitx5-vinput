#pragma once

#include <cstdint>
#include <unistd.h>
#include <utility>

namespace vinput::file {

// Exclusive RAII owner for a POSIX file descriptor.
class UniqueFd {
public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { Reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  [[nodiscard]] int Get() const noexcept { return fd_; }
  [[nodiscard]] bool IsValid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return IsValid(); }

  int Release() noexcept { return std::exchange(fd_, -1); }

  void Reset(int new_fd = -1) noexcept {
    if (fd_ >= 0 && fd_ != new_fd) {
      close(fd_);
    }
    fd_ = new_fd;
  }

private:
  int fd_ = -1;
};

inline void SignalEventFd(int fd) noexcept {
  if (fd >= 0) {
    const std::uint64_t val = 1;
    (void)write(fd, &val, sizeof(val));
  }
}

inline void DrainEventFd(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  std::uint64_t val = 0;
  while (read(fd, &val, sizeof(val)) == sizeof(val)) {
  }
}

} // namespace vinput::file
