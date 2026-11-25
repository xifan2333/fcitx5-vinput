#include "common/net/http_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <curl/curlver.h>
#include <curl/easy.h>
#include <curl/system.h>
#include <curl/urlapi.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "common/net/http_types.h"
#include "common/utils/scope_guard.h"
#include "common/utils/string_utils.h"

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
    if (vinput::str::EqualsIgnoreCase(h.name, "authorization")) {
      return h.value;
    }
  }
  return {};
}

bool HasHeader(const std::vector<HttpHeader>& headers, std::string_view name) {
  return std::ranges::any_of(
      headers, [name](const HttpHeader& h) { return vinput::str::EqualsIgnoreCase(h.name, name); });
}

bool IsCancelRequested(const std::atomic<bool>* token) noexcept {
  return token != nullptr && token->load(std::memory_order_relaxed);
}

// Normalizes a file:// URL via libcurl's URL API: raw local paths with spaces or
// non-ASCII bytes are percent-encoded (RFC 8089 / RFC 3986 pchar). Returns the
// original string when the URL API is unavailable (libcurl < 7.62) or rejects input.
std::string NormalizeFileUrl(const std::string& url) {
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> handle(curl_url(), &curl_url_cleanup);
  if (!handle) {
    return url;
  }

  int flags = CURLU_URLENCODE;
#if LIBCURL_VERSION_NUM >= 0x074E00
  flags |= CURLU_ALLOW_SPACE;
#endif
  if (curl_url_set(handle.get(), CURLUPART_URL, url.c_str(), flags) != CURLUE_OK) {
    return url;
  }

  std::unique_ptr<char, decltype(&curl_free)> encoded(nullptr, &curl_free);
  char* raw_url = nullptr;
  if (curl_url_get(handle.get(), CURLUPART_URL, &raw_url, 0) != CURLUE_OK) {
    return url;
  }
  encoded.reset(raw_url);
  return std::string(encoded.get());
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
  std::string body;
  size_t max_bytes = 0;
  bool overflow = false;
};

size_t MemoryWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  const size_t total = size * nmemb;
  if (userdata == nullptr || ptr == nullptr || total == 0) {
    return 0;
  }
  auto* ctx = static_cast<MemoryWriteContext*>(userdata);
  if (total > ctx->max_bytes || ctx->body.size() > ctx->max_bytes - total) {
    ctx->overflow = true;
    return 0;
  }
  ctx->body.append(ptr, total);
  return total;
}

struct FileWriteContext {
  std::ofstream* stream = nullptr;
  bool failed = false;
};

using ResponseSink = std::variant<MemoryWriteContext, FileWriteContext>;

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

// Captures headers of the final response only: a new status line marks the start of
// the next response in a redirect chain, so previously captured headers are dropped.
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
  if (line.starts_with("HTTP/")) {
    headers->clear();
    return total;
  }
  const auto colon = line.find(':');
  if (colon != std::string_view::npos) {
    const std::string name(line.substr(0, colon));
    std::string_view val = line.substr(colon + 1);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
      val.remove_prefix(1);
    }
    headers->push_back({name, std::string(val)});
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

double ComputeTransferSpeed(TransferProgressContext* ctx, uint64_t current_bytes) {
  const auto now = std::chrono::steady_clock::now();
  double speed_bps = 0.0;
  if (ctx->has_last) {
    const double elapsed = std::chrono::duration<double>(now - ctx->last_time).count();
    if (elapsed > 0.0 && current_bytes >= ctx->last_bytes) {
      speed_bps = static_cast<double>(current_bytes - ctx->last_bytes) / elapsed;
    }
  }
  ctx->last_bytes = current_bytes;
  ctx->last_time = now;
  ctx->has_last = true;
  return speed_bps;
}

int TransferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                         curl_off_t ulnow) {
  auto* ctx = static_cast<TransferProgressContext*>(clientp);
  if (ctx == nullptr) {
    return 0;
  }
  if (IsCancelRequested(ctx->cancel_token)) {
    return 1;
  }
  if (!ctx->callback) {
    return 0;
  }

  HttpProgress progress;
  progress.downloaded_bytes = dlnow > 0 ? static_cast<uint64_t>(dlnow)
                                        : static_cast<uint64_t>(std::max<curl_off_t>(ulnow, 0));
  progress.total_bytes = dltotal > 0 ? static_cast<uint64_t>(dltotal)
                                     : static_cast<uint64_t>(std::max<curl_off_t>(ultotal, 0));
  progress.speed_bps = ComputeTransferSpeed(ctx, progress.downloaded_bytes);

  try {
    ctx->callback(progress);
  } catch (...) {
    ctx->callback_exception = true;
    return 1;
  }
  return IsCancelRequested(ctx->cancel_token) ? 1 : 0;
}

// RAII wrapper for curl_slist header chains.
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
  CurlHeaderList(CurlHeaderList&& other) noexcept : list(std::exchange(other.list, nullptr)) {}
  CurlHeaderList& operator=(CurlHeaderList&&) = delete;

  void Append(const std::string& str) { list = curl_slist_append(list, str.c_str()); }
};

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

void ConfigureAllowedProtocols(CURL* curl) {
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,file");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS | CURLPROTO_FILE));
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
}

void ConfigureMethod(CURL* curl, const HttpRequest& request, CurlBodyReader* body_reader) {
  const bool use_read_callback = !request.allow_auto_resend;
  switch (request.method) {
  case HttpMethod::Get:
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    return;
  case HttpMethod::Post:
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    break;
  case HttpMethod::Put:
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    break;
  case HttpMethod::Delete:
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    if (request.body.empty()) {
      return;
    }
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    break;
  case HttpMethod::Head:
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    return;
  }

  if (use_read_callback) {
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadRequestBodyCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, body_reader);
  } else {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
  }
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
}

// Applies the request body's default Content-Type per method when the caller did not
// provide one. Explicit caller values (including the empty value) pass through untouched.
void AppendDefaultContentType(CurlHeaderList* header_list, const HttpRequest& request) {
  if (request.method == HttpMethod::Get || request.method == HttpMethod::Head ||
      HasHeader(request.headers, "Content-Type")) {
    return;
  }
  const char* default_type = request.method == HttpMethod::Post
                                 ? "application/x-www-form-urlencoded"
                                 : "application/octet-stream";
  header_list->Append(std::string("Content-Type: ") + default_type);
}

CurlHeaderList BuildHeaderList(const HttpRequest& request) {
  CurlHeaderList header_list;
  for (const auto& h : request.headers) {
    // curl_slist semantics: "Name:" removes the header, "Name;" sends an empty value.
    if (h.value.empty()) {
      header_list.Append(h.name + ";");
    } else {
      header_list.Append(h.name + ": " + h.value);
    }
  }
  if (!HasHeader(request.headers, "user-agent")) {
    header_list.Append("User-Agent: fcitx5-vinput");
  }
  if (!HasHeader(request.headers, "expect")) {
    header_list.Append("Expect:");
  }
  AppendDefaultContentType(&header_list, request);
  return header_list;
}

void ConfigureProxyAndProgress(CURL* curl, const HttpRequest& request,
                               TransferProgressContext* prog_ctx) {
  if (request.proxy.has_value()) {
    if (request.proxy->empty()) {
      curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    } else {
      curl_easy_setopt(curl, CURLOPT_PROXY, request.proxy->c_str());
    }
  }
  if (request.cancel_token != nullptr || request.progress_cb != nullptr) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TransferInfoCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, prog_ctx);
  } else {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  }
}

void ConfigureWriteSink(CURL* curl, ResponseSink* sink) {
  if (auto* mem = std::get_if<MemoryWriteContext>(sink)) {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, MemoryWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, mem);
  } else if (auto* file = std::get_if<FileWriteContext>(sink)) {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FileWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  }
}

void PopulateResponseResult(CURL* curl, CURLcode code, const HttpRequest& request,
                            const TransferProgressContext& prog_ctx, const ResponseSink& sink,
                            HttpResponse* response) {
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status_code);
  const char* effective_url = nullptr;
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
  if (effective_url != nullptr && *effective_url != '\0') {
    response->effective_url = effective_url;
  }
  double total_sec = 0.0;
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_sec);
  response->total_time_ms = total_sec * 1000.0;

  if (code == CURLE_OK && response->status_code == 0 && request.url.starts_with("file://")) {
    response->status_code = 200;
  }

  const auto* mem = std::get_if<MemoryWriteContext>(&sink);
  const bool overflow = mem != nullptr && mem->overflow;

  if (prog_ctx.callback_exception) {
    response->error_message = "Progress callback threw an exception";
  } else if (IsCancelRequested(request.cancel_token)) {
    response->cancelled = true;
    response->error_message = "Request cancelled";
  } else if (code == CURLE_WRITE_ERROR && overflow) {
    response->error_message = "Response exceeded maximum allowed size";
  } else if (code != CURLE_OK) {
    response->error_message = curl_easy_strerror(code);
  }
}

} // namespace

class HttpClient::Impl {
public:
  using CurlHandlePtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

  Impl() { EnsureCurlGlobalInit(); }

  ~Impl() {
    const std::scoped_lock lock(pool_mutex_);
    pool_.clear();
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  struct HandleLease {
    CurlHandlePtr handle{nullptr, &curl_easy_cleanup};
    ConnectionIdentity identity;
  };

  HttpResponse Execute(const HttpRequest& request, ResponseSink* sink) {
    if (!EnsureCurlGlobalInit()) {
      HttpResponse res;
      res.error_message = "Failed to globally initialize libcurl";
      return res;
    }

    if (IsCancelRequested(request.cancel_token)) {
      HttpResponse res;
      res.cancelled = true;
      res.error_message = "Request cancelled";
      return res;
    }

    const ConnectionIdentity id{
        .origin = ExtractOrigin(request.url),
        .credential = ExtractAuthorization(request.headers),
        .proxy_policy =
            request.proxy.has_value() ? ("custom=" + *request.proxy) : BuildProxyPolicySignature(),
    };

    HandleLease lease = Acquire(id);
    if (!lease.handle) {
      HttpResponse res;
      res.error_message = "Failed to initialize libcurl handle";
      return res;
    }

    CURL* curl = lease.handle.get();
    const CurlHeaderList header_list = BuildHeaderList(request);
    CurlBodyReader body_reader{.body = request.body, .offset = 0};
    HttpResponse response;
    TransferProgressContext prog_ctx;
    prog_ctx.cancel_token = request.cancel_token;
    prog_ctx.callback = request.progress_cb;

    // libcurl 7.78+ rejects file:// URLs containing raw spaces and non-ASCII
    // characters; normalize the URL via libcurl's own URL API before transfer.
    std::string request_url = request.url;
    if (request_url.starts_with("file://")) {
      request_url = NormalizeFileUrl(request_url);
    }

    curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list.list);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, request.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    ConfigureAllowedProtocols(curl);
    ConfigureMethod(curl, request, &body_reader);
    ConfigureWriteSink(curl, sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderWriteCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    ConfigureProxyAndProgress(curl, request, &prog_ctx);

    const CURLcode code = curl_easy_perform(curl);
    PopulateResponseResult(curl, code, request, prog_ctx, *sink, &response);

    // Handles tainted by a failed transfer never return to the pool; only fully
    // completed transfers are eligible for keep-alive reuse.
    if (code == CURLE_OK && !IsCancelRequested(request.cancel_token)) {
      Release(std::move(lease));
    }
    return response;
  }

private:
  HandleLease Acquire(const ConnectionIdentity& identity) {
    const std::scoped_lock lock(pool_mutex_);
    auto it = pool_.find(identity);
    if (it != pool_.end() && !it->second.empty()) {
      CurlHandlePtr handle = std::move(it->second.back());
      it->second.pop_back();
      curl_easy_reset(handle.get());
      return {.handle = std::move(handle), .identity = identity};
    }
    return {.handle = CurlHandlePtr(curl_easy_init(), &curl_easy_cleanup), .identity = identity};
  }

  void Release(HandleLease lease) {
    if (!lease.handle) {
      return;
    }
    const std::scoped_lock lock(pool_mutex_);
    auto& handles = pool_[lease.identity];
    if (handles.size() < kMaxPoolPerIdentity) {
      handles.push_back(std::move(lease.handle));
    }
  }

  static constexpr std::size_t kMaxPoolPerIdentity = 4;
  mutable std::mutex pool_mutex_;
  std::unordered_map<ConnectionIdentity, std::vector<CurlHandlePtr>, ConnectionIdentityHash> pool_;
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}
HttpClient::~HttpClient() = default;

HttpResponse HttpClient::Request(const HttpRequest& request) {
  ResponseSink sink(std::in_place_type<MemoryWriteContext>);
  std::get<MemoryWriteContext>(sink).max_bytes = request.max_response_bytes;
  HttpResponse res = impl_->Execute(request, &sink);
  res.body = std::move(std::get<MemoryWriteContext>(sink).body);
  return res;
}

HttpResponse HttpClient::PostJson(const std::string& url, const std::string& json_body,
                                  const std::vector<HttpHeader>& extra_headers, long timeout_ms,
                                  const std::atomic<bool>* cancel_token) {
  HttpRequest req;
  req.url = url;
  req.method = HttpMethod::Post;
  req.body = json_body;
  req.headers = extra_headers;
  if (!req.Header("Content-Type").has_value()) {
    req.headers.push_back({"Content-Type", "application/json"});
  }
  req.timeout_ms = timeout_ms;
  req.cancel_token = cancel_token;
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
  auto temp_guard = vinput::MakeScopeExit([&temp_path]() {
    std::error_code rm_ec;
    std::filesystem::remove(temp_path, rm_ec);
  });

  std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    HttpResponse res;
    res.error_message = "Failed to open destination file for writing: " + temp_path.string();
    return res;
  }

  ResponseSink sink(std::in_place_type<FileWriteContext>, FileWriteContext{.stream = &out});
  HttpResponse res = impl_->Execute(request, &sink);
  out.flush();
  const bool stream_good = out.good();
  out.close();

  const bool file_failed = std::get<FileWriteContext>(sink).failed;
  if (!res.Ok() || res.cancelled || file_failed || !stream_good) {
    if (res.error_message.empty()) {
      res.error_message = "Failed to write complete download stream";
    }
    return res;
  }

  std::filesystem::rename(temp_path, dest_path, ec);
  if (ec) {
    res.error_message = "Failed to commit downloaded file: " + ec.message();
    return res;
  }
  temp_guard.Dismiss();
  return res;
}

HttpClient& HttpClient::Instance() {
  static HttpClient instance;
  return instance;
}

} // namespace vinput::net
