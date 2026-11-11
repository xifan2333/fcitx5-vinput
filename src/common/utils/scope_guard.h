#pragma once

#include <type_traits>
#include <utility>

namespace vinput {

template <typename F> class ScopeGuard {
public:
  template <typename Fn>
    requires(!std::is_same_v<std::remove_cvref_t<Fn>, ScopeGuard>)
  explicit constexpr ScopeGuard(Fn&& fn) noexcept(std::is_nothrow_constructible_v<F, Fn>)
      : fn_(std::forward<Fn>(fn)) {}

  ~ScopeGuard() noexcept {
    if (active_) {
      fn_();
    }
  }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

  constexpr ScopeGuard(ScopeGuard&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
      : fn_(std::move(other.fn_)), active_(std::exchange(other.active_, false)) {}

  ScopeGuard& operator=(ScopeGuard&&) = delete;

  void Dismiss() noexcept { active_ = false; }

private:
  F fn_;
  bool active_ = true;
};

template <typename F> [[nodiscard]] constexpr auto MakeScopeExit(F&& fn) {
  return ScopeGuard<std::decay_t<F>>(std::forward<F>(fn));
}

} // namespace vinput
