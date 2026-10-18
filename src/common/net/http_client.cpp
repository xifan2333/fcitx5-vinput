#include "common/net/http_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/system.h>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/net/http_types.h"

namespace vinput::net {

namespace {

bool EnsureCurlGlobalInit() {
  static std::once_flag flag;
  static bool initialized = false;
  std::call_once(flag, []() { initialized = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK); });
  return initialized;
}

std::string BuildProxyPolicySignature() {
  static constexpr std::array<const char*, 11> kVariables = {
      "http_proxy", "HTTP_PROXY", "https_proxy",    "HTTPS_PROXY",   "all_proxy",   "ALL_PROXY",
      "no_proxy",   "NO_PROXY",   "CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR"};
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

std::string ExtractOrigin(std::string_view url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    return std::string(url);
  }
  const auto path_start = url.find_first_of("/?#", scheme_end + 3);
  if (path_start == std::string_view::npos) {
    return std::string(url);
  }
  return std::string(url.substr(0, path_start));
}

std::string ExtractAuthorization(const std::vector<HttpHeader>& headers) {
  for (const auto& h : headers) {
    if (h.name.size() == 13) {
      std::string lower;
      lower.reserve(13);
      for (const char c : h.name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      if (lower == "authorization") {
        return h.value;
      }
    }
  }
  return {};
}

bool HasUserAgent(const std::vector<HttpHeader>& headers) {
  for (const auto& h : headers) {
    if (h.name.size() == 10) {
      std::string lower;
      lower.reserve(10);
      for (const char c : h.name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      if (lower == "user-agent") {
        return true;
      }
    }
  }
  return false;
}

struct CurlBodyReader {
  std::string_view body;
  size_t offset = 0;
};

size_t ReadRequestBodyCallback(char* buffer, size_t size, size_t nmemb, void* userdata) {
  auto* reader = static_cast<CurlBodyReader*>(userdata);
  if (reader == nullptr) {
    return CURL_READFUNC_ABORT;
  }
  const size_t remaining = reader->body.size() - reader->offset;
  const size_t to_copy = std::min(remaining, size * nmemb);
  if (to_copy > 0) {
    std::memcpy(buffer, reader->body.data() + reader->offset, to_copy);
    reader->offset += to_copy;
  }
  return to_copy;
}

struct MemoryWriteContext {
  std::string* body = nullptr;
  size_t max_bytes = 0;
  bool overflow = false;
};

size_t MemoryWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  const size_t total = size * nmemb;
  if (userdata == nullptr || ptr == nullptr || total == 0) {
    return 0;
  }
  auto* ctx = static_cast<MemoryWriteContext*>(userdata);
  if (ctx->body->size() + total > ctx->max_bytes) {
    ctx->overflow = true;
    return 0;
  }
  ctx->body->append(ptr, total);
  return total;
}

struct FileWriteContext {
  std::ofstream* stream = nullptr;
  bool failed = false;
};

size_t FileWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  const size_t total = size * nmemb;
  if (userdata == nullptr || ptr == nullptr || total == 0) {
    return 0;
  }
  auto* ctx = static_cast<FileWriteContext*>(userdata);
  ctx->stream->write(ptr, static_cast<std::streamsize>(total));
  if (!ctx->stream->good()) {
    ctx->failed = true;
    return 0;
  }
  return total;
}

size_t HeaderWriteCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
  const size_t total = size * nitems;
  auto* headers = static_cast<std::vector<HttpHeader>*>(userdata);
  if (headers == nullptr || buffer == nullptr) {
    return total;
  }
  std::string_view line(buffer, total);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  const auto colon = line.find(':');
  if (colon != std::string_view::npos) {
    std::string name(line.substr(0, colon));
    std::string_view val = line.substr(colon + 1);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
      val.remove_prefix(1);
    }
    headers->push_back({std::move(name), std::string(val)});
  }
  return total;
}

struct TransferProgressContext {
  const std::atomic<bool>* cancel_token = nullptr;
  ProgressCallback callback = nullptr;
  std::chrono::steady_clock::time_point last_time;
  uint64_t last_bytes = 0;
  bool has_last = false;
  bool callback_exception = false;
};

int TransferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                         curl_off_t ulnow) {
  auto* ctx = static_cast<TransferProgressContext*>(clientp);
  if (ctx == nullptr) {
    return 0;
  }
  if (ctx->cancel_token != nullptr && ctx->cancel_token->load(std::memory_order_relaxed)) {
    return 1;
  }
  if (ctx->callback) {
    HttpProgress progress;
    uint64_t dl_bytes = 0;
    if (dlnow > 0) {
      dl_bytes = static_cast<uint64_t>(dlnow);
    } else if (ulnow > 0) {
      dl_bytes = static_cast<uint64_t>(ulnow);
    }
    progress.downloaded_bytes = dl_bytes;

    uint64_t tot_bytes = 0;
    if (dltotal > 0) {
      tot_bytes = static_cast<uint64_t>(dltotal);
    } else if (ultotal > 0) {
      tot_bytes = static_cast<uint64_t>(ultotal);
    }
    progress.total_bytes = tot_bytes;

    const auto now = std::chrono::steady_clock::now();
    if (ctx->has_last) {
      const double elapsed = std::chrono::duration<double>(now - ctx->last_time).count();
      if (elapsed > 0.0 && progress.downloaded_bytes >= ctx->last_bytes) {
        progress.speed_bps =
            static_cast<double>(progress.downloaded_bytes - ctx->last_bytes) / elapsed;
      }
    }
    ctx->last_bytes = progress.downloaded_bytes;
    ctx->last_time = now;
    ctx->has_last = true;
    try {
      ctx->callback(progress);
    } catch (...) {
      ctx->callback_exception = true;
      return 1;
    }
  }
  return 0;
}

struct CurlHeaderList {
  curl_slist* list = nullptr;
  CurlHeaderList() = default;
  ~CurlHeaderList() {
    if (list != nullptr) {
      curl_slist_free_all(list);
    }
  }
  CurlHeaderList(const CurlHeaderList&) = delete;
  CurlHeaderList& operator=(const CurlHeaderList&) = delete;
  CurlHeaderList(CurlHeaderList&&) = delete;
  CurlHeaderList& operator=(CurlHeaderList&&) = delete;

  void Append(const std::string& str) { list = curl_slist_append(list, str.c_str()); }
};

struct ConnectionIdentity {
  std::string origin;
  std::string credential;
  std::string proxy_policy;

  bool operator==(const ConnectionIdentity&) const = default;
};

} // namespace

class HttpClient::Impl {
public:
  Impl() { EnsureCurlGlobalInit(); }

  ~Impl() {
    const std::scoped_lock lock(mutex_);
    for (auto* h : pool_) {
      if (h != nullptr) {
        curl_easy_cleanup(h);
      }
    }
    pool_.clear();
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  struct HandleLease {
    CURL* handle = nullptr;
    ConnectionIdentity identity;
  };

  HandleLease Acquire(const ConnectionIdentity& identity) {
    const std::scoped_lock lock(mutex_);
    if (active_identity_ == identity && !pool_.empty()) {
      CURL* h = pool_.back();
      pool_.pop_back();
      curl_easy_reset(h);
      return {h, identity};
    }

    if (active_identity_ != identity) {
      for (auto* h : pool_) {
        if (h != nullptr) {
          curl_easy_cleanup(h);
        }
      }
      pool_.clear();
      active_identity_ = identity;
    }

    CURL* h = curl_easy_init();
    return {h, identity};
  }

  void Release(const HandleLease& lease) {
    if (lease.handle == nullptr) {
      return;
    }
    const std::scoped_lock lock(mutex_);
    if (active_identity_ == lease.identity && pool_.size() < kMaxPoolSize) {
      curl_easy_reset(lease.handle);
      pool_.push_back(lease.handle);
    } else {
      curl_easy_cleanup(lease.handle);
    }
  }

  HttpResponse Execute(const HttpRequest& request, size_t (*write_cb)(char*, size_t, size_t, void*),
                       void* write_data, const bool* overflow_flag) {
    if (!EnsureCurlGlobalInit()) {
      HttpResponse res;
      res.error_message = "Failed to globally initialize libcurl";
      return res;
    }

    const ConnectionIdentity id{
        .origin = ExtractOrigin(request.url),
        .credential = ExtractAuthorization(request.headers),
        .proxy_policy =
            request.proxy.has_value() ? ("custom=" + *request.proxy) : BuildProxyPolicySignature(),
    };

    const HandleLease lease = Acquire(id);
    if (lease.handle == nullptr) {
      HttpResponse res;
      res.error_message = "Failed to initialize libcurl handle";
      return res;
    }

    CURL* curl = lease.handle;
    CurlHeaderList header_list;
    for (const auto& h : request.headers) {
      header_list.Append(h.name + ": " + h.value);
    }
    if (!HasUserAgent(request.headers)) {
      header_list.Append("User-Agent: fcitx5-vinput");
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list.list);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, request.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    if (request.proxy.has_value()) {
      curl_easy_setopt(curl, CURLOPT_PROXY, request.proxy->c_str());
    }

    CurlBodyReader body_reader{.body = request.body, .offset = 0};
    switch (request.method) {
    case HttpMethod::Get:
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
      break;
    case HttpMethod::Post:
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      if (request.allow_auto_resend) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
      } else {
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadRequestBodyCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &body_reader);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
      }
      break;
    case HttpMethod::Put:
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
      if (request.allow_auto_resend) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
      } else {
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadRequestBodyCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &body_reader);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
      }
      break;
    case HttpMethod::Delete:
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
      if (!request.body.empty()) {
        if (request.allow_auto_resend) {
          curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
          curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        } else {
          curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadRequestBodyCallback);
          curl_easy_setopt(curl, CURLOPT_READDATA, &body_reader);
          curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        }
      }
      break;
    case HttpMethod::Head:
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
      break;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_data);

    HttpResponse response;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderWriteCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    TransferProgressContext prog_ctx;
    prog_ctx.cancel_token = request.cancel_token;
    prog_ctx.callback = request.progress_cb;
    if (request.cancel_token != nullptr || request.progress_cb != nullptr) {
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TransferInfoCallback);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog_ctx);
    } else {
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }

    const CURLcode code = curl_easy_perform(curl);

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    char* effective_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    if (effective_url != nullptr && *effective_url != '\0') {
      response.effective_url = effective_url;
    }
    double total_sec = 0.0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_sec);
    response.total_time_ms = total_sec * 1000.0;

    if (code == CURLE_OK && response.status_code == 0 && request.url.starts_with("file://")) {
      response.status_code = 200;
    }

    if (prog_ctx.callback_exception) {
      response.error_message = "Progress callback threw an exception";
    } else if (code == CURLE_ABORTED_BY_CALLBACK && request.cancel_token != nullptr &&
               request.cancel_token->load(std::memory_order_relaxed)) {
      response.cancelled = true;
      response.error_message = "Request cancelled";
    } else if (code == CURLE_WRITE_ERROR && overflow_flag != nullptr && *overflow_flag) {
      response.error_message = "Response exceeded maximum allowed size";
    } else if (code != CURLE_OK) {
      response.error_message = curl_easy_strerror(code);
    }

    Release(lease);
    return response;
  }

private:
  static constexpr size_t kMaxPoolSize = 4;
  std::mutex mutex_;
  ConnectionIdentity active_identity_;
  std::vector<CURL*> pool_;
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}
HttpClient::~HttpClient() = default;

HttpResponse HttpClient::Request(const HttpRequest& request) {
  HttpResponse response;
  MemoryWriteContext ctx{
      .body = &response.body, .max_bytes = request.max_response_bytes, .overflow = false};
  HttpResponse exec_res = impl_->Execute(request, MemoryWriteCallback, &ctx, &ctx.overflow);
  exec_res.body = std::move(response.body);
  return exec_res;
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
  req.timeout_ms = timeout_ms;
  req.cancel_token = cancel_token;
  req.headers.push_back({"Content-Type", "application/json"});
  for (const auto& h : extra_headers) {
    req.headers.push_back(h);
  }
  return Request(req);
}

HttpResponse HttpClient::DownloadToFile(const HttpRequest& request,
                                        const std::filesystem::path& dest_path) {
  std::error_code ec;
  const auto parent = dest_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      HttpResponse res;
      res.error_message = "Failed to create directory: " + ec.message();
      return res;
    }
  }

  const std::filesystem::path temp_path =
      dest_path.string() + ".tmp." +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    HttpResponse res;
    res.error_message = "Failed to open destination file for writing: " + temp_path.string();
    return res;
  }

  FileWriteContext ctx{.stream = &out, .failed = false};
  HttpResponse res = impl_->Execute(request, FileWriteCallback, &ctx, nullptr);
  out.flush();
  const bool stream_good = out.good();
  out.close();

  if (res.Ok() && !res.cancelled && !ctx.failed && stream_good) {
    std::filesystem::rename(temp_path, dest_path, ec);
    if (ec) {
      std::filesystem::remove(temp_path, ec);
      res.error_message = "Failed to commit downloaded file: " + ec.message();
    }
  } else {
    std::filesystem::remove(temp_path, ec);
    if (res.error_message.empty()) {
      res.error_message = "Failed to write complete download stream";
    }
  }
  return res;
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
