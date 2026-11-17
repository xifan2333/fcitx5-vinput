#pragma once

#include <signal.h> // IWYU pragma: export

namespace vinput {

/// RAII guard for a POSIX signal handler installed via sigaction(2).
/// Restores the previous disposition on destruction.
class SignalGuard {
public:
  using Handler = void (*)(int);

  explicit SignalGuard(int signum, Handler handler) noexcept : signum_(signum) {
    struct sigaction sa{};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (::sigaction(signum_, &sa, &old_act_) == 0) {
      installed_ = true;
    }
  }

  ~SignalGuard() noexcept {
    if (installed_) {
      ::sigaction(signum_, &old_act_, nullptr);
    }
  }

  SignalGuard(const SignalGuard&) = delete;
  SignalGuard& operator=(const SignalGuard&) = delete;
  SignalGuard(SignalGuard&&) = delete;
  SignalGuard& operator=(SignalGuard&&) = delete;

  [[nodiscard]] bool Installed() const noexcept { return installed_; }

private:
  int signum_{-1};
  struct sigaction old_act_{};
  bool installed_{false};
};

} // namespace vinput
