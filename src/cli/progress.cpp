#include "cli/progress.h"

#include <cstdio>
#include <cstring>

static std::string FormatSpeed(double speed_bps) {
  if (speed_bps <= 0) return "";
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
    : label_(label), total_(total), is_tty_(is_tty), last_percent_(-1) {}

void ProgressBar::Update(uint64_t current, double speed_bps) {
  if (total_ == 0) return;

  int percent = static_cast<int>((current * 100) / total_);
  if (percent > 100) percent = 100;

  if (is_tty_) {
    // Build bar: [=====>    ] 65% (1.2MB/s)
    constexpr int bar_width = 20;
    int filled = (percent * bar_width) / 100;
    char bar[bar_width + 1];
    for (int i = 0; i < bar_width; ++i) {
      if (i < filled - 1) bar[i] = '=';
      else if (i == filled - 1 && filled > 0) bar[i] = '>';
      else bar[i] = ' ';
    }
    bar[bar_width] = '\0';

    std::string speed_str = FormatSpeed(speed_bps);
    if (!speed_str.empty()) {
      fprintf(stderr, "\r%s [%s] %d%% (%s)   ",
              label_.c_str(), bar, percent, speed_str.c_str());
    } else {
      fprintf(stderr, "\r%s [%s] %d%%   ",
              label_.c_str(), bar, percent);
    }
    fflush(stderr);
    last_percent_ = percent;
  } else {
    // Non-TTY: print every 10%
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
  if (is_tty_) {
    fprintf(stderr, "\n");
  } else if (last_percent_ < 100) {
    fprintf(stderr, "%s: 100%%\n", label_.c_str());
  } else {
    fprintf(stderr, "\n");
  }
  fflush(stderr);
}
