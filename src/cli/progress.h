#pragma once

#include <cstdint>
#include <string>

class ProgressBar {
public:
  ProgressBar(const std::string& label, uint64_t total, bool is_tty);
  void Update(uint64_t current, double speed_bps = 0);
  void Finish();

private:
  std::string label_;
  uint64_t total_;
  bool is_tty_;
  int last_percent_ = -1;
};
