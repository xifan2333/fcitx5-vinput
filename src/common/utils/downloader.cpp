#include "common/utils/downloader.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/net/http_client.h"
#include "common/net/http_types.h"

namespace vinput::download {

namespace {

vinput::net::ProgressCallback MakeProgressCallback(const ProgressCallback& cb) {
  if (!cb) {
    return nullptr;
  }
  return [cb](const vinput::net::HttpProgress& hp) {
    Progress p;
    p.downloaded_bytes = hp.downloaded_bytes;
    p.total_bytes = hp.total_bytes;
    p.speed_bps = hp.speed_bps;
    cb(p);
  };
}

vinput::net::HttpRequest MakeRequest(const std::string& url, const Options& options) {
  vinput::net::HttpRequest req;
  req.url = url;
  req.method = vinput::net::HttpMethod::Get;
  req.timeout_ms = options.timeout_seconds * 1000;
  req.follow_redirects = options.follow_redirects;
  req.cancel_token = options.cancel_token;
  req.progress_cb = MakeProgressCallback(options.progress_cb);
  return req;
}

void PopulateResult(const std::string& url, const vinput::net::HttpResponse& res, Result* result) {
  if (!result) {
    return;
  }
  result->resolved_url = url;
  result->http_code = res.status_code;
  result->ok = res.Ok();
  if (res.Ok()) {
    result->error.clear();
  } else if (res.cancelled) {
    result->error = "download cancelled";
  } else if (res.status_code != 0) {
    result->error = "download failed with HTTP " + std::to_string(res.status_code);
  } else if (!res.error_message.empty()) {
    result->error = "download failed: " + res.error_message;
  } else {
    result->error = "download failed";
  }
}

bool IsCancelled(const Options& options, const vinput::net::HttpResponse& res) {
  return res.cancelled ||
         (options.cancel_token != nullptr && options.cancel_token->load(std::memory_order_relaxed));
}

} // namespace

bool DownloadText(const std::vector<std::string>& urls, const Options& options,
                  std::string* content, Result* result) {
  if (content) {
    content->clear();
  }

  Result last_result;
  for (const auto& url : urls) {
    if (options.cancel_token && options.cancel_token->load(std::memory_order_relaxed)) {
      last_result.ok = false;
      last_result.resolved_url = url;
      last_result.error = "download cancelled";
      break;
    }
    if (url.empty()) {
      continue;
    }

    auto req = MakeRequest(url, options);
    req.max_response_bytes =
        options.max_bytes > 0 ? options.max_bytes : std::numeric_limits<size_t>::max();

    const auto res = vinput::net::HttpClient::Instance().Request(req);
    PopulateResult(url, res, &last_result);
    last_result.bytes_received = res.body.size();

    if (res.Ok()) {
      if (content) {
        *content = std::move(res.body);
      }
      if (result) {
        *result = std::move(last_result);
      }
      return true;
    }

    if (IsCancelled(options, res)) {
      break;
    }
  }

  if (result) {
    *result = std::move(last_result);
  }
  return false;
}

bool DownloadFile(const std::vector<std::string>& urls, const std::filesystem::path& dest,
                  const Options& options, Result* result) {
  Result last_result;
  for (const auto& url : urls) {
    if (options.cancel_token && options.cancel_token->load(std::memory_order_relaxed)) {
      last_result.ok = false;
      last_result.resolved_url = url;
      last_result.error = "download cancelled";
      break;
    }
    if (url.empty()) {
      continue;
    }

    const auto req = MakeRequest(url, options);
    const auto res = vinput::net::HttpClient::Instance().DownloadToFile(req, dest);
    PopulateResult(url, res, &last_result);

    if (res.Ok()) {
      std::error_code ec;
      const auto sz = std::filesystem::file_size(dest, ec);
      last_result.bytes_received = ec ? 0 : sz;
      if (result) {
        *result = std::move(last_result);
      }
      return true;
    }

    last_result.bytes_received = 0;
    if (IsCancelled(options, res)) {
      break;
    }
  }

  if (result) {
    *result = std::move(last_result);
  }
  return false;
}

} // namespace vinput::download
