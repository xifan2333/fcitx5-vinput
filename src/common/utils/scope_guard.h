#pragma once

#include <type_traits>
#include <unistd.h>
#include <utility>

namespace vinput {

/**
 * Standard-conforming ScopeExit (WG21 P0052):
 * Invokes the given callable on scope exit unless explicitly dismissed.
 */
template <typename F> class ScopeExit {
public:
  template <typename Fn>
    requires(!std::is_same_v<std::remove_cvref_t<Fn>, ScopeExit>)
  explicit ScopeExit(Fn&& fn) : fn_(std::forward<Fn>(fn)) {}

  ~ScopeExit() noexcept {
    if (active_) {
      fn_();
    }
  }

  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

  ScopeExit(ScopeExit&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
      : fn_(std::move(other.fn_)), active_(other.active_) {
    other.active_ = false;
  }

  ScopeExit& operator=(ScopeExit&& other) noexcept {
    if (this != &other) {
      if (active_) {
        fn_();
      }
      fn_ = std::move(other.fn_);
      active_ = other.active_;
      other.active_ = false;
    }
    return *this;
  }

  void release() noexcept { active_ = false; }
  void dismiss() noexcept { active_ = false; }

private:
  F fn_;
  bool active_ = true;
};

template <typename F> ScopeExit(F) -> ScopeExit<F>;

template <typename F> [[nodiscard]] auto make_scope_exit(F&& f) {
  return ScopeExit<std::decay_t<F>>(std::forward<F>(f));
}

/**
 * UniqueFd: RAII wrapper for POSIX file descriptors.
 * Prevents fd leakage across branches and exceptions.
 */
class UniqueFd {
public:
  constexpr UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() noexcept { reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return is_valid(); }

  [[nodiscard]] int release() noexcept {
    const int old = fd_;
    fd_ = -1;
    return old;
  }

  void reset(int new_fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

private:
  int fd_ = -1;
};

} // namespace vinput
