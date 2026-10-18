#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vinput::net {

enum class HttpMethod : std::uint8_t {
  Get,
  Post,
  Put,
  Delete,
  Head,
};

struct HttpHeader {
  std::string name;
  std::string value;

  bool operator==(const HttpHeader&) const = default;
};

struct HttpProgress {
  uint64_t downloaded_bytes = 0;
  uint64_t total_bytes = 0;
  double speed_bps = 0.0;
};

using ProgressCallback = std::function<void(const HttpProgress&)>;

struct HttpRequest {
  std::string url;
  HttpMethod method = HttpMethod::Get;
  std::vector<HttpHeader> headers;
  std::string body;
  long timeout_ms = 30000;
  long connect_timeout_ms = 5000;
  bool follow_redirects = true;
  size_t max_response_bytes = 10ULL * 1024ULL * 1024ULL; // 10 MB default defensive limit
  std::optional<std::string> proxy; // nullopt: env default; "": bypass; set: custom
  const std::atomic<bool>* cancel_token = nullptr;
  ProgressCallback progress_cb = nullptr;
  bool allow_auto_resend = false; // False by default to prevent silent POST retry on disconnect
};

struct HttpResponse {
  long status_code = 0;
  std::string body;
  std::vector<HttpHeader> headers;
  std::string effective_url;
  double total_time_ms = 0.0;
  std::string error_message;
  bool cancelled = false;

  [[nodiscard]] bool Ok() const noexcept {
    return status_code >= 200 && status_code < 300 && error_message.empty() && !cancelled;
  }
};

} // namespace vinput::net
