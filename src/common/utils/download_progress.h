#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace indicators {
class ProgressBar;
} // namespace indicators

class ProgressBar {
public:
  ProgressBar(std::string label, uint64_t total, bool is_tty);
  ProgressBar(ProgressBar&&) noexcept;
  ProgressBar& operator=(ProgressBar&&) noexcept;
  ~ProgressBar();

  ProgressBar(const ProgressBar&) = delete;
  ProgressBar& operator=(const ProgressBar&) = delete;

  void Update(uint64_t current, double speed_bps = 0);
  /// Finalizes the progress display. If the transfer never reported 100%, the
  /// bar is left at its last reported percentage instead of being forced to
  /// look complete (e.g. after an aborted download).
  void Finish();

private:
  std::string label_;
  uint64_t total_;
  bool is_tty_;
  int last_percent_ = -1;
  std::unique_ptr<indicators::ProgressBar> bar_;
};
