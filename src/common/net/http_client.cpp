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
#include <sstream>
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

    // 1. Unified File protocol handler using vinput::file infrastructure
    if (uc.scheme == "file") {
      HttpResponse resp;
      const std::filesystem::path file_path(uc.path);
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

    // 2. HTTP/HTTPS protocol handler
    std::string host = uc.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
      host = "[" + host + "]";
    }
    const std::string origin = uc.scheme + "://" + host + (uc.port.empty() ? "" : ":" + uc.port);
    const std::string path_and_query = (uc.path.empty() ? "/" : uc.path) + uc.query;

    const ConnectionIdentity id{
        .origin = origin,
        .credential = ExtractAuthorization(request.headers),
        .proxy_policy =
            request.proxy.has_value() ? ("custom=" + *request.proxy) : BuildProxyPolicySignature(),
    };

    std::shared_ptr<httplib::Client> cli = Acquire(id);
    const bool new_client = !cli;
    if (!cli) {
      cli = std::make_shared<httplib::Client>(origin);
    }

    // Explicitly enable persistent keep-alive connections on client
    cli->set_keep_alive(true);
    cli->set_follow_location(request.follow_redirects);
    cli->set_read_timeout(request.timeout_ms / 1000, (request.timeout_ms % 1000) * 1000);
    cli->set_connection_timeout(request.connect_timeout_ms / 1000,
                                (request.connect_timeout_ms % 1000) * 1000);
    cli->set_write_timeout(request.timeout_ms / 1000, (request.timeout_ms % 1000) * 1000);

    // In cpp-httplib, payload_max_length of 0 means unlimited.
    // For file downloads (streamed directly to disk), do not clamp with in-memory limit.
    if (dest_path != nullptr) {
      cli->set_payload_max_length(0);
    } else {
      cli->set_payload_max_length(request.max_response_bytes);
    }

    auto apply_proxy = [&](std::string_view proxy_str) {
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
        cli->set_proxy(p_uc.host, p_port);
      }
    };

    if (new_client && request.proxy.has_value()) {
      if (!request.proxy->empty()) {
        apply_proxy(*request.proxy);
      }
    } else if (new_client) {
      const char* ambient_proxy = nullptr;
      if (uc.scheme == "https") {
        ambient_proxy = std::getenv("https_proxy");
        if (ambient_proxy == nullptr) {
          ambient_proxy = std::getenv("HTTPS_PROXY");
        }
      } else {
        ambient_proxy = std::getenv("http_proxy");
        if (ambient_proxy == nullptr) {
          ambient_proxy = std::getenv("HTTP_PROXY");
        }
      }
      if (ambient_proxy == nullptr) {
        ambient_proxy = std::getenv("all_proxy");
        if (ambient_proxy == nullptr) {
          ambient_proxy = std::getenv("ALL_PROXY");
        }
      }
      if (ambient_proxy != nullptr && *ambient_proxy != '\0') {
        apply_proxy(ambient_proxy);
      }

      // Delegate no_proxy matching completely to cpp-httplib native set_no_proxy()
      const char* no_proxy = std::getenv("no_proxy");
      if (no_proxy == nullptr) {
        no_proxy = std::getenv("NO_PROXY");
      }
      if (no_proxy != nullptr && *no_proxy != '\0') {
        std::vector<std::string> patterns;
        std::stringstream ss(no_proxy);
        for (std::string item; std::getline(ss, item, ',');) {
          const std::string trimmed(vinput::str::Trim(item));
          if (!trimmed.empty()) {
            patterns.push_back(trimmed);
          }
        }
        if (!patterns.empty()) {
          cli->set_no_proxy(patterns);
        }
      }
    }

    httplib::Headers headers;
    for (const auto& h : request.headers) {
      headers.emplace(h.name, h.value);
    }
    if (!HasUserAgent(request.headers)) {
      headers.emplace("User-Agent", "fcitx5-vinput");
    }

    struct ProgressTracker {
      std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
      std::uint64_t last_bytes = 0;
      bool has_last = false;
    } tracker;

    auto progress_fn = [&](size_t current, size_t total) -> bool {
      if (request.cancel_token != nullptr &&
          request.cancel_token->load(std::memory_order_relaxed)) {
        return false;
      }
      if (request.progress_cb) {
        HttpProgress p;
        p.downloaded_bytes = current;
        p.total_bytes = total;
        const auto now = std::chrono::steady_clock::now();
        if (tracker.has_last) {
          const double elapsed = std::chrono::duration<double>(now - tracker.last_time).count();
          if (elapsed > 0.0 && p.downloaded_bytes >= tracker.last_bytes) {
            p.speed_bps = static_cast<double>(p.downloaded_bytes - tracker.last_bytes) / elapsed;
          }
        }
        tracker.last_bytes = p.downloaded_bytes;
        tracker.last_time = now;
        tracker.has_last = true;
        try {
          request.progress_cb(p);
        } catch (...) {
          return false;
        }
        if (request.cancel_token != nullptr &&
            request.cancel_token->load(std::memory_order_relaxed)) {
          return false;
        }
      }
      return true;
    };

    if (request.cancel_token != nullptr && request.cancel_token->load(std::memory_order_relaxed)) {
      HttpResponse resp;
      resp.cancelled = true;
      resp.error_message = "Request cancelled";
      Release(id, cli);
      return resp;
    }

    const auto start_time = std::chrono::steady_clock::now();
    httplib::Result res;

    if (dest_path != nullptr) {
      std::error_code ec;
      std::filesystem::create_directories(dest_path->parent_path(), ec);

      const auto tmp_path =
          dest_path->parent_path() /
          (dest_path->filename().string() + ".tmp." +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::ofstream out_file(tmp_path, std::ios::binary);
      if (!out_file.is_open()) {
        HttpResponse resp;
        resp.error_message = "Failed to open destination file: " + tmp_path.string();
        Release(id, cli);
        return resp;
      }

      res = cli->Get(
          path_and_query, headers,
          [&](const char* data, size_t data_length) {
            if (request.cancel_token != nullptr &&
                request.cancel_token->load(std::memory_order_relaxed)) {
              return false;
            }
            out_file.write(data, static_cast<std::streamsize>(data_length));
            return out_file.good();
          },
          progress_fn);
      out_file.flush();
      out_file.close();

      if (res && res->status >= 200 && res->status < 300) {
        std::filesystem::rename(tmp_path, *dest_path, ec);
        if (ec) {
          std::filesystem::remove(tmp_path, ec);
        }
      } else {
        std::filesystem::remove(tmp_path, ec);
      }
    } else {
      switch (request.method) {
      case HttpMethod::Get:
        res = cli->Get(path_and_query, headers, progress_fn);
        break;
      case HttpMethod::Post: {
        std::string ct = "application/x-www-form-urlencoded";
        const auto ct_opt = request.Header("Content-Type");
        if (ct_opt.has_value()) {
          ct = std::string(*ct_opt);
        }
        res = cli->Post(path_and_query, headers, request.body, ct, progress_fn);
        break;
      }
      case HttpMethod::Put: {
        std::string ct = "application/octet-stream";
        const auto ct_opt = request.Header("Content-Type");
        if (ct_opt.has_value()) {
          ct = std::string(*ct_opt);
        }
        res = cli->Put(path_and_query, headers, request.body, ct, progress_fn);
        break;
      }
      case HttpMethod::Delete: {
        std::string ct = "application/octet-stream";
        const auto ct_opt = request.Header("Content-Type");
        if (ct_opt.has_value()) {
          ct = std::string(*ct_opt);
        }
        res = cli->Delete(path_and_query, headers, request.body, ct, progress_fn);
        break;
      }
      case HttpMethod::Head:
        res = cli->Head(path_and_query, headers);
        break;
      }
    }

    const auto end_time = std::chrono::steady_clock::now();
    HttpResponse response;
    response.total_time_ms =
        std::chrono::duration<double, std::milli>(end_time - start_time).count();

    if (request.cancel_token != nullptr && request.cancel_token->load(std::memory_order_relaxed)) {
      response.cancelled = true;
      response.error_message = "Request cancelled";
      Release(id, cli);
      return response;
    }

    if (res) {
      response.status_code = res->status;
      response.body = std::move(res->body);
      for (const auto& [k, v] : res->headers) {
        response.headers.push_back({k, v});
      }
      const auto loc = res->get_header_value("Location");
      if (!loc.empty()) {
        response.effective_url = loc.starts_with('/') ? (origin + loc) : loc;
      } else if (!res->location.empty()) {
        response.effective_url =
            res->location.starts_with('/') ? (origin + res->location) : res->location;
      } else {
        response.effective_url = request.url;
      }
      Release(id, cli);
    } else {
      const auto err = res.error();
      if (err == httplib::Error::Canceled ||
          (request.cancel_token != nullptr &&
           request.cancel_token->load(std::memory_order_relaxed))) {
        response.cancelled = true;
        response.error_message = "Request cancelled";
      } else if (err == httplib::Error::ExceedMaxPayloadSize) {
        response.error_message = "Response exceeded maximum allowed size";
      } else {
        response.error_message = httplib::to_string(err);
      }
    }

    return response;
  }

private:
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
  req.headers = extra_headers;
  req.headers.push_back({"Content-Type", "application/json"});
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
