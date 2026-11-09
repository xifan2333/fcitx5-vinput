#include "common/net/http_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <httplib.h>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/net/http_types.h"
#include "common/utils/file_utils.h"
#include "common/utils/string_utils.h"

namespace vinput::net {

namespace {

std::string BuildProxyPolicySignature() {
  static constexpr std::array<const char*, 8> kVariables = {
      "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY",
      "all_proxy",  "ALL_PROXY",  "no_proxy",    "NO_PROXY"};
  std::string signature;
  for (const char* name : kVariables) {
    const char* value = std::getenv(name);
    if (value != nullptr) {
      signature.append(name);
      signature.push_back('=');
      signature.append(value);
      signature.push_back('\0');
    }
  }
  return signature;
}

std::string ExtractAuthorization(const std::vector<HttpHeader>& headers) {
  for (const auto& h : headers) {
    if (vinput::str::EqualsIgnoreCase(h.name, "authorization")) {
      return h.value;
    }
  }
  return {};
}

bool HasUserAgent(const std::vector<HttpHeader>& headers) {
  return std::ranges::any_of(
      headers, [](const auto& h) { return vinput::str::EqualsIgnoreCase(h.name, "user-agent"); });
}

bool IsCancelled(const HttpRequest& request) noexcept {
  return request.cancel_token != nullptr && request.cancel_token->load(std::memory_order_relaxed);
}

struct ConnectionIdentity {
  std::string origin;
  std::string credential;
  std::string proxy_policy;

  bool operator==(const ConnectionIdentity&) const = default;
};

struct ConnectionIdentityHash {
  std::size_t operator()(const ConnectionIdentity& id) const noexcept {
    return std::hash<std::string>{}(id.origin) ^ (std::hash<std::string>{}(id.credential) << 1) ^
           (std::hash<std::string>{}(id.proxy_policy) << 2);
  }
};

} // namespace

class HttpClient::Impl {
public:
  using ProgressFn = httplib::DownloadProgress;

  struct ProgressTracker {
    std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
    std::uint64_t last_bytes = 0;
    bool has_last = false;

    void Update(size_t current, size_t total, const HttpRequest& request) {
      HttpProgress p;
      p.downloaded_bytes = current;
      p.total_bytes = total;
      const auto now = std::chrono::steady_clock::now();
      if (has_last) {
        const double elapsed = std::chrono::duration<double>(now - last_time).count();
        if (elapsed > 0.0 && p.downloaded_bytes >= last_bytes) {
          p.speed_bps = static_cast<double>(p.downloaded_bytes - last_bytes) / elapsed;
        }
      }
      last_bytes = p.downloaded_bytes;
      last_time = now;
      has_last = true;
      request.progress_cb(p);
    }
  };

  static ProgressFn SetupProgressTracker(const HttpRequest& request, ProgressTracker& tracker) {
    return [&request, &tracker](size_t current, size_t total) -> bool {
      if (IsCancelled(request)) {
        return false;
      }
      if (request.progress_cb) {
        try {
          tracker.Update(current, total, request);
        } catch (...) {
          return false;
        }
        if (IsCancelled(request)) {
          return false;
        }
      }
      return true;
    };
  }

  Impl() = default;
  ~Impl() = default;

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  HttpResponse Execute(const HttpRequest& request, const std::filesystem::path* dest_path) {
    httplib::detail::UrlComponents uc;
    if (!httplib::detail::parse_url(request.url, uc)) {
      HttpResponse resp;
      resp.error_message = "Invalid URL: " + request.url;
      return resp;
    }

    if (uc.scheme == "file") {
      return HandleFileProtocol(request, dest_path, uc.path);
    }

    const std::string origin = FormatOrigin(uc);
    const std::string path_and_query = (uc.path.empty() ? "/" : uc.path) + uc.query;

    const ConnectionIdentity id{
        .origin = origin,
        .credential = ExtractAuthorization(request.headers),
        .proxy_policy =
            request.proxy.has_value() ? ("custom=" + *request.proxy) : BuildProxyPolicySignature(),
    };

    auto cli = AcquireOrCreateClient(id, request, dest_path, uc.scheme);
    const httplib::Headers headers = BuildHeaders(request);
    ProgressTracker tracker;
    auto progress_fn = SetupProgressTracker(request, tracker);

    if (IsCancelled(request)) {
      HttpResponse resp;
      resp.cancelled = true;
      resp.error_message = "Request cancelled";
      Release(id, cli);
      return resp;
    }

    const auto start_time = std::chrono::steady_clock::now();
    httplib::Result res;

    if (dest_path != nullptr) {
      auto dl =
          ExecuteStreamingDownload(*cli, path_and_query, headers, request, *dest_path, progress_fn);
      if (!dl.error.empty()) {
        HttpResponse resp;
        resp.error_message = std::move(dl.error);
        Release(id, cli);
        return resp;
      }
      res = std::move(dl.res);
    } else {
      res = DispatchMethodRequest(*cli, request, path_and_query, headers, progress_fn);
    }

    const auto end_time = std::chrono::steady_clock::now();
    HttpResponse response;
    response.total_time_ms =
        std::chrono::duration<double, std::milli>(end_time - start_time).count();

    if (IsCancelled(request)) {
      response.cancelled = true;
      response.error_message = "Request cancelled";
      Release(id, cli);
      return response;
    }

    ExtractResponseMetadata(res, request, origin, response);
    if (res) {
      Release(id, cli);
    }
    return response;
  }

private:
  std::shared_ptr<httplib::Client> AcquireOrCreateClient(const ConnectionIdentity& id,
                                                         const HttpRequest& request,
                                                         const std::filesystem::path* dest_path,
                                                         std::string_view scheme) {
    std::shared_ptr<httplib::Client> cli = Acquire(id);
    if (!cli) {
      cli = std::make_shared<httplib::Client>(id.origin);
      ApplyProxyConfiguration(*cli, request, scheme);
    }
    ConfigureClientOptions(*cli, request, dest_path);
    return cli;
  }

  static HttpResponse HandleFileProtocol(const HttpRequest& request,
                                         const std::filesystem::path* dest_path,
                                         const std::string& path) {
    HttpResponse resp;
    const std::filesystem::path file_path(path);
    if (dest_path != nullptr) {
      std::error_code ec;
      std::filesystem::create_directories(dest_path->parent_path(), ec);
      std::filesystem::copy_file(file_path, *dest_path,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        resp.status_code = 404;
        resp.error_message = "Failed to copy local file: " + ec.message();
        return resp;
      }
    } else {
      auto read_res = vinput::file::ReadTextFile(file_path, request.max_response_bytes);
      if (!read_res) {
        resp.status_code = 404;
        resp.error_message = read_res.error();
        return resp;
      }
      resp.body = std::move(*read_res);
    }
    resp.status_code = 200;
    resp.effective_url = request.url;
    return resp;
  }

  static std::string FormatOrigin(const httplib::detail::UrlComponents& uc) {
    std::string host = uc.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
      host = "[" + host + "]";
    }
    return uc.scheme + "://" + host + (uc.port.empty() ? "" : ":" + uc.port);
  }

  static void ApplyProxyUrl(httplib::Client& cli, std::string_view proxy_str) {
    std::string p_url(proxy_str);
    if (!p_url.starts_with("http://") && !p_url.starts_with("https://")) {
      p_url = "http://" + p_url;
    }
    httplib::detail::UrlComponents p_uc;
    if (httplib::detail::parse_url(p_url, p_uc)) {
      int p_port = 80;
      if (!p_uc.port.empty()) {
        p_port = std::atoi(p_uc.port.c_str());
      } else if (p_uc.scheme == "https") {
        p_port = 443;
      }
      cli.set_proxy(p_uc.host, p_port);
    }
  }

  static const char* GetFirstEnv(std::span<const char* const> names) {
    for (const char* name : names) {
      const char* val = std::getenv(name);
      if (val != nullptr && *val != '\0') {
        return val;
      }
    }
    return nullptr;
  }

  static const char* ResolveAmbientProxy(std::string_view scheme) {
    static constexpr std::array<const char*, 4> kHttpsVars = {"https_proxy", "HTTPS_PROXY",
                                                              "all_proxy", "ALL_PROXY"};
    static constexpr std::array<const char*, 4> kHttpVars = {"http_proxy", "HTTP_PROXY",
                                                             "all_proxy", "ALL_PROXY"};
    return GetFirstEnv(scheme == "https" ? kHttpsVars : kHttpVars);
  }

  static void ApplyNoProxyPatterns(httplib::Client& cli) {
    static constexpr std::array<const char*, 2> kNoProxyVars = {"no_proxy", "NO_PROXY"};
    const char* no_proxy = GetFirstEnv(kNoProxyVars);
    if (no_proxy == nullptr) {
      return;
    }
    std::vector<std::string> patterns;
    for (const auto item : vinput::str::Split(no_proxy, ',')) {
      const auto trimmed = vinput::str::Trim(item);
      if (!trimmed.empty()) {
        patterns.emplace_back(trimmed);
      }
    }
    if (!patterns.empty()) {
      cli.set_no_proxy(patterns);
    }
  }

  static void ApplyProxyConfiguration(httplib::Client& cli, const HttpRequest& request,
                                      std::string_view scheme) {
    if (request.proxy.has_value()) {
      if (!request.proxy->empty()) {
        ApplyProxyUrl(cli, *request.proxy);
      }
      return;
    }
    const char* ambient = ResolveAmbientProxy(scheme);
    if (ambient != nullptr) {
      ApplyProxyUrl(cli, ambient);
    }
    ApplyNoProxyPatterns(cli);
  }

  static void ConfigureClientOptions(httplib::Client& cli, const HttpRequest& request,
                                     const std::filesystem::path* dest_path) {
    cli.set_keep_alive(true);
    cli.set_follow_location(request.follow_redirects);
    cli.set_read_timeout(request.timeout_ms / 1000, (request.timeout_ms % 1000) * 1000);
    cli.set_connection_timeout(request.connect_timeout_ms / 1000,
                               (request.connect_timeout_ms % 1000) * 1000);
    cli.set_write_timeout(request.timeout_ms / 1000, (request.timeout_ms % 1000) * 1000);
    cli.set_payload_max_length(dest_path != nullptr ? 0 : request.max_response_bytes);
  }

  static httplib::Headers BuildHeaders(const HttpRequest& request) {
    httplib::Headers headers;
    for (const auto& h : request.headers) {
      headers.emplace(h.name, h.value);
    }
    if (!HasUserAgent(request.headers)) {
      headers.emplace("User-Agent", "fcitx5-vinput");
    }
    return headers;
  }

  struct DownloadResult {
    httplib::Result res;
    std::string error;
  };

  static DownloadResult
  ExecuteStreamingDownload(httplib::Client& cli, const std::string& path_and_query,
                           const httplib::Headers& headers, const HttpRequest& request,
                           const std::filesystem::path& dest_path, const ProgressFn& progress_fn) {
    std::error_code ec;
    std::filesystem::create_directories(dest_path.parent_path(), ec);

    const auto tmp_path =
        dest_path.parent_path() /
        (dest_path.filename().string() + ".tmp." +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::ofstream out_file(tmp_path, std::ios::binary);
    if (!out_file.is_open()) {
      return {.res = {}, .error = "Failed to open destination file: " + tmp_path.string()};
    }

    auto res = cli.Get(
        path_and_query, headers,
        [&](const char* data, size_t data_length) {
          if (IsCancelled(request)) {
            return false;
          }
          out_file.write(data, static_cast<std::streamsize>(data_length));
          return out_file.good();
        },
        progress_fn);
    out_file.flush();
    out_file.close();

    if (res && res->status >= 200 && res->status < 300) {
      std::filesystem::rename(tmp_path, dest_path, ec);
      if (ec) {
        std::filesystem::remove(tmp_path, ec);
      }
    } else {
      std::filesystem::remove(tmp_path, ec);
    }
    return {.res = std::move(res), .error = {}};
  }

  static httplib::Result DispatchMethodRequest(httplib::Client& cli, const HttpRequest& request,
                                               const std::string& path_and_query,
                                               const httplib::Headers& headers,
                                               const ProgressFn& progress_fn) {
    switch (request.method) {
    case HttpMethod::Get:
      return cli.Get(path_and_query, headers, progress_fn);
    case HttpMethod::Post: {
      const std::string ct =
          request.Header("Content-Type").has_value() ? "" : "application/x-www-form-urlencoded";
      return cli.Post(path_and_query, headers, request.body, ct, progress_fn);
    }
    case HttpMethod::Put: {
      const std::string ct =
          request.Header("Content-Type").has_value() ? "" : "application/octet-stream";
      return cli.Put(path_and_query, headers, request.body, ct, progress_fn);
    }
    case HttpMethod::Delete: {
      const std::string ct =
          request.Header("Content-Type").has_value() ? "" : "application/octet-stream";
      return cli.Delete(path_and_query, headers, request.body, ct, progress_fn);
    }
    case HttpMethod::Head:
      return cli.Head(path_and_query, headers);
    }
    return {};
  }

  static std::string ExtractEffectiveUrl(const httplib::Response& res, const std::string& origin,
                                         const std::string& default_url) {
    const auto loc = res.get_header_value("Location");
    if (!loc.empty()) {
      return loc.starts_with('/') ? (origin + loc) : loc;
    }
    if (!res.location.empty()) {
      return res.location.starts_with('/') ? (origin + res.location) : res.location;
    }
    return default_url;
  }

  static void ExtractSuccessResponse(httplib::Response& res, const HttpRequest& request,
                                     const std::string& origin, HttpResponse& response) {
    response.status_code = res.status;
    response.body = std::move(res.body);
    for (const auto& [k, v] : res.headers) {
      response.headers.push_back({k, v});
    }
    response.effective_url = ExtractEffectiveUrl(res, origin, request.url);
  }

  static void ExtractErrorResponse(httplib::Error err, const HttpRequest& request,
                                   HttpResponse& response) {
    if (err == httplib::Error::Canceled || IsCancelled(request)) {
      response.cancelled = true;
      response.error_message = "Request cancelled";
      return;
    }
    if (err == httplib::Error::ExceedMaxPayloadSize) {
      response.error_message = "Response exceeded maximum allowed size";
      return;
    }
    response.error_message = httplib::to_string(err);
  }

  static void ExtractResponseMetadata(httplib::Result& res, const HttpRequest& request,
                                      const std::string& origin, HttpResponse& response) {
    if (res) {
      ExtractSuccessResponse(*res, request, origin, response);
    } else {
      ExtractErrorResponse(res.error(), request, response);
    }
  }

  std::shared_ptr<httplib::Client> Acquire(const ConnectionIdentity& id) {
    const std::scoped_lock lock(pool_mutex_);
    auto it = pool_.find(id);
    if (it != pool_.end() && !it->second.empty()) {
      auto cli = it->second.back();
      it->second.pop_back();
      return cli;
    }
    return nullptr;
  }

  void Release(const ConnectionIdentity& id, const std::shared_ptr<httplib::Client>& cli) {
    if (!cli) {
      return;
    }
    const std::scoped_lock lock(pool_mutex_);
    if (pool_[id].size() < kMaxPoolPerHost) {
      pool_[id].push_back(cli);
    }
  }

  static constexpr std::size_t kMaxPoolPerHost = 4;
  mutable std::mutex pool_mutex_;
  std::unordered_map<ConnectionIdentity, std::vector<std::shared_ptr<httplib::Client>>,
                     ConnectionIdentityHash>
      pool_;
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}
HttpClient::~HttpClient() = default;

HttpResponse HttpClient::Request(const HttpRequest& request) {
  return impl_->Execute(request, nullptr);
}

HttpResponse HttpClient::Get(const std::string& url, long timeout_ms,
                             const std::atomic<bool>* cancel_token) {
  HttpRequest req;
  req.url = url;
  req.method = HttpMethod::Get;
  req.timeout_ms = timeout_ms;
  req.cancel_token = cancel_token;
  return Request(req);
}

HttpResponse HttpClient::Post(const std::string& url, const std::string& body,
                              const std::vector<HttpHeader>& headers, long timeout_ms,
                              const std::atomic<bool>* cancel_token) {
  HttpRequest req;
  req.url = url;
  req.method = HttpMethod::Post;
  req.body = body;
  req.headers = headers;
  req.timeout_ms = timeout_ms;
  req.cancel_token = cancel_token;
  return Request(req);
}

HttpResponse HttpClient::PostJson(const std::string& url, const std::string& json_body,
                                  const std::vector<HttpHeader>& extra_headers, long timeout_ms,
                                  const std::atomic<bool>* cancel_token) {
  HttpRequest req;
  req.url = url;
  req.method = HttpMethod::Post;
  req.body = json_body;
  req.headers.reserve(extra_headers.size() + 1);

  bool has_content_type = false;
  for (const auto& h : extra_headers) {
    if (vinput::str::EqualsIgnoreCase(h.name, "Content-Type")) {
      if (!has_content_type) {
        req.headers.push_back(h);
        has_content_type = true;
      }
    } else {
      req.headers.push_back(h);
    }
  }
  if (!has_content_type) {
    req.headers.push_back({"Content-Type", "application/json"});
  }

  req.timeout_ms = timeout_ms;
  req.cancel_token = cancel_token;
  return Request(req);
}

HttpResponse HttpClient::DownloadToFile(const HttpRequest& request,
                                        const std::filesystem::path& dest_path) {
  return impl_->Execute(request, &dest_path);
}

HttpResponse HttpClient::DownloadToFile(const std::string& url,
                                        const std::filesystem::path& dest_path,
                                        ProgressCallback progress_cb,
                                        const std::atomic<bool>* cancel_token) {
  HttpRequest req;
  req.url = url;
  req.method = HttpMethod::Get;
  req.progress_cb = std::move(progress_cb);
  req.cancel_token = cancel_token;
  return DownloadToFile(req, dest_path);
}

HttpClient& HttpClient::Instance() {
  static HttpClient instance;
  return instance;
}

} // namespace vinput::net
