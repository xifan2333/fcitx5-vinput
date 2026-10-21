#include "common/utils/download_progress.h"

#include <cstddef>
#include <cstdio>
#include <indicators/indicators.hpp>
#include <iostream>
#include <string>

static std::string FormatSpeed(double speed_bps) {
  if (speed_bps <= 0) {
    return "";
  }
  char buf[32];
  if (speed_bps >= 1024.0 * 1024.0) {
    snprintf(buf, sizeof(buf), "%.1fMB/s", speed_bps / (1024.0 * 1024.0));
  } else if (speed_bps >= 1024.0) {
    snprintf(buf, sizeof(buf), "%.1fKB/s", speed_bps / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%.0fB/s", speed_bps);
  }
  return buf;
}

ProgressBar::ProgressBar(const std::string& label, uint64_t total, bool is_tty)
    : label_(label), total_(total), is_tty_(is_tty), last_percent_(-1) {
  if (is_tty_ && total_ > 0) {
    bar_ = std::make_unique<indicators::ProgressBar>(
        indicators::option::BarWidth{30}, indicators::option::PrefixText{label_},
        indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{false},
        indicators::option::ShowRemainingTime{false}, indicators::option::Fill{"█"},
        indicators::option::Lead{"█"}, indicators::option::Remainder{"░"},
        indicators::option::ForegroundColor{indicators::Color::green},
        indicators::option::MaxProgress{100}, indicators::option::Stream{std::cerr});
  }
}

// Move/destructor must be defined in the .cpp where indicators::ProgressBar is complete
ProgressBar::ProgressBar(ProgressBar&&) noexcept = default;
ProgressBar& ProgressBar::operator=(ProgressBar&&) noexcept = default;
ProgressBar::~ProgressBar() = default;

void ProgressBar::Update(uint64_t current, double speed_bps) {
  if (total_ == 0) {
    return;
  }

  int percent = static_cast<int>((current * 100) / total_);
  if (percent > 100) {
    percent = 100;
  }

  if (is_tty_ && bar_) {
    std::string speed_str = FormatSpeed(speed_bps);
    if (!speed_str.empty()) {
      bar_->set_option(indicators::option::PostfixText{"(" + speed_str + ")"});
    }
    bar_->set_progress(static_cast<size_t>(percent));
    last_percent_ = percent;
  } else {
    // Non-TTY: 10% bucket line output for CI/log readability
    int bucket = (percent / 10) * 10;
    int last_bucket = (last_percent_ < 0) ? -1 : (last_percent_ / 10) * 10;
    if (bucket > last_bucket) {
      fprintf(stderr, "%s: %d%%\n", label_.c_str(), bucket);
      fflush(stderr);
      last_percent_ = percent;
    }
  }
}

void ProgressBar::Finish() {
  if (is_tty_ && bar_) {
    if (!bar_->is_completed()) {
      bar_->set_progress(100);
      bar_->mark_as_completed();
    }
    // indicators prints newline on completion; no extra newline needed
  } else if (last_percent_ < 100) {
    fprintf(stderr, "%s: 100%%\n", label_.c_str());
    fflush(stderr);
  }
}
