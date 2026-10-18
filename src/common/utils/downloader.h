#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vinput::download {

struct Progress {
  uint64_t downloaded_bytes = 0;
  uint64_t total_bytes = 0;
  double speed_bps = 0;
};

using ProgressCallback = std::function<void(const Progress&)>;

struct Options {
  long timeout_seconds = 30;
  size_t max_bytes = 0;
  bool follow_redirects = true;
  ProgressCallback progress_cb;
  const std::atomic<bool>* cancel_token = nullptr;
  std::optional<std::string> proxy = std::nullopt;
};

struct Result {
  bool ok = false;
  std::string resolved_url;
  long http_code = 0;
  size_t bytes_received = 0;
  std::string error;
};

bool DownloadText(const std::vector<std::string>& urls, const Options& options,
                  std::string* content, Result* result = nullptr);
bool DownloadFile(const std::vector<std::string>& urls, const std::filesystem::path& dest,
                  const Options& options, Result* result = nullptr);

} // namespace vinput::download
