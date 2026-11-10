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
  if (cb == nullptr) {
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
  req.connect_timeout_ms = options.timeout_seconds * 1000;
  req.follow_redirects = options.follow_redirects;
  req.cancel_token = options.cancel_token;
  req.progress_cb = MakeProgressCallback(options.progress_cb);
  req.proxy = options.proxy;
  return req;
}

bool RequiresHttpStatusCheck(const std::string& url) {
  return url.starts_with("http://") || url.starts_with("https://");
}

void PopulateResult(const std::string& url, const vinput::net::HttpResponse& res, Result* result) {
  if (result == nullptr) {
    return;
  }
  result->resolved_url = res.effective_url.empty() ? url : res.effective_url;
  result->http_code = res.status_code;

  const bool http_ok = RequiresHttpStatusCheck(url) ? (res.status_code == 200) : true;
  result->ok = res.Ok() && http_ok;

  if (result->ok) {
    result->error.clear();
  } else if (res.cancelled) {
    result->error = "download cancelled";
  } else if (!res.error_message.empty()) {
    result->error = "download failed: " + res.error_message;
  } else if (RequiresHttpStatusCheck(url) && res.status_code != 200 && res.status_code != 0) {
    result->error = "download failed with HTTP " + std::to_string(res.status_code);
  } else {
    result->error = "download failed";
  }
}

bool IsCancelled(const Options& options, const vinput::net::HttpResponse& res) {
  return res.cancelled ||
         (options.cancel_token != nullptr && options.cancel_token->load(std::memory_order_relaxed));
}

bool TryHandlePreDownloadCancellation(const Options& options, const std::string& url,
                                      Result* result) {
  if (options.cancel_token != nullptr && options.cancel_token->load(std::memory_order_relaxed)) {
    result->ok = false;
    result->resolved_url = url;
    result->error = "download cancelled";
    return true;
  }
  return false;
}

template <typename RequestFn>
bool ExecuteDownloadLoop(const std::vector<std::string>& urls, const Options& options,
                         Result* out_result, RequestFn&& request_fn) {
  Result last_result;
  for (const auto& url : urls) {
    if (TryHandlePreDownloadCancellation(options, url, &last_result)) {
      break;
    }
    if (url.empty()) {
      continue;
    }

    const auto [success, cancelled] = request_fn(url, &last_result);
    if (success) {
      if (out_result != nullptr) {
        *out_result = std::move(last_result);
      }
      return true;
    }

    if (cancelled) {
      break;
    }
  }

  if (out_result != nullptr) {
    *out_result = std::move(last_result);
  }
  return false;
}

} // namespace

bool DownloadText(const std::vector<std::string>& urls, const Options& options,
                  std::string* content, Result* result) {
  if (content != nullptr) {
    content->clear();
  }

  return ExecuteDownloadLoop(
      urls, options, result, [&](const std::string& url, Result* last_result) {
        auto req = MakeRequest(url, options);
        req.max_response_bytes =
            options.max_bytes > 0 ? options.max_bytes : std::numeric_limits<size_t>::max();

        auto res = vinput::net::HttpClient::Instance().Request(req);
        PopulateResult(url, res, last_result);
        last_result->bytes_received = res.body.size();

        if (last_result->ok) {
          if (content != nullptr) {
            *content = std::move(res.body);
          }
          return std::make_pair(true, false);
        }

        return std::make_pair(false, IsCancelled(options, res));
      });
}

bool DownloadFile(const std::vector<std::string>& urls, const std::filesystem::path& dest,
                  const Options& options, Result* result) {
  const bool ok =
      ExecuteDownloadLoop(urls, options, result, [&](const std::string& url, Result* last_result) {
        const auto req = MakeRequest(url, options);
        auto res = vinput::net::HttpClient::Instance().DownloadToFile(req, dest);
        PopulateResult(url, res, last_result);

        if (last_result->ok) {
          std::error_code ec;
          const auto sz = std::filesystem::file_size(dest, ec);
          last_result->bytes_received = ec ? 0 : sz;
          return std::make_pair(true, false);
        }

        last_result->bytes_received = 0;
        return std::make_pair(false, IsCancelled(options, res));
      });

  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(dest, ec);
  }
  return ok;
}

} // namespace vinput::download
