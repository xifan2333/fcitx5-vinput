#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/net/http_types.h"

namespace vinput::net {

class HttpClient {
public:
  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;
  HttpClient(HttpClient&&) = delete;
  HttpClient& operator=(HttpClient&&) = delete;

  // Unified HTTP request entrypoint
  HttpResponse Request(const HttpRequest& request);

  // Convenience methods
  HttpResponse Get(const std::string& url, long timeout_ms = 10000,
                   const std::atomic<bool>* cancel_token = nullptr);

  HttpResponse Post(const std::string& url, const std::string& body,
                    const std::vector<HttpHeader>& headers = {}, long timeout_ms = 30000,
                    const std::atomic<bool>* cancel_token = nullptr);

  HttpResponse PostJson(const std::string& url, const std::string& json_body,
                        const std::vector<HttpHeader>& extra_headers = {}, long timeout_ms = 30000,
                        const std::atomic<bool>* cancel_token = nullptr);

  // Dedicated chunked file download to disk
  HttpResponse DownloadToFile(const HttpRequest& request, const std::filesystem::path& dest_path);

  HttpResponse DownloadToFile(const std::string& url, const std::filesystem::path& dest_path,
                              ProgressCallback progress_cb = nullptr,
                              const std::atomic<bool>* cancel_token = nullptr);

  // Global shared singleton (thread-safe with connection pooling)
  static HttpClient& Instance();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vinput::net
