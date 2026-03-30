#include "common/utils/downloader.h"

#include <curl/curl.h>

#include <chrono>
#include <fstream>
#include <string>

#include "common/utils/file_utils.h"

namespace vinput::download {

namespace {

struct MemoryBuffer {
  std::string data;
  size_t max_size = 0;
};

struct FileWriter {
  std::ofstream *out = nullptr;
};

struct ProgressState {
  ProgressCallback cb;
  uint64_t last_downloaded_bytes = 0;
  std::chrono::steady_clock::time_point last_update_time{};
  bool has_last_update = false;
};

size_t WriteToMemory(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<MemoryBuffer *>(userdata);
  const size_t total = size * nmemb;
  if (buf->max_size > 0 && buf->data.size() + total > buf->max_size) {
    return 0;
  }
  buf->data.append(ptr, total);
  return total;
}

size_t WriteToFile(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *writer = static_cast<FileWriter *>(userdata);
  const size_t total = size * nmemb;
  writer->out->write(ptr, static_cast<std::streamsize>(total));
  if (!writer->out->good()) {
    return 0;
  }
  return total;
}

int ProgressCallbackFn(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  auto *state = static_cast<ProgressState *>(userdata);
  if (!state->cb) {
    return 0;
  }

  const uint64_t downloaded_bytes = static_cast<uint64_t>(dlnow);
  const auto now = std::chrono::steady_clock::now();
  Progress progress;
  progress.downloaded_bytes = downloaded_bytes;
  progress.total_bytes = static_cast<uint64_t>(dltotal > 0 ? dltotal : 0);
  if (state->has_last_update) {
    const auto elapsed = now - state->last_update_time;
    const double elapsed_seconds =
        std::chrono::duration<double>(elapsed).count();
    if (elapsed_seconds > 0 &&
        downloaded_bytes >= state->last_downloaded_bytes) {
      progress.speed_bps =
          (downloaded_bytes - state->last_downloaded_bytes) / elapsed_seconds;
    }
  }
  state->last_downloaded_bytes = downloaded_bytes;
  state->last_update_time = now;
  state->has_last_update = true;
  state->cb(progress);
  return 0;
}

bool RequiresHttpStatusCheck(const std::string &url) {
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool FinalizeResult(const std::string &attempt_url, CURL *curl, CURLcode code,
                    size_t bytes_received, Result *result) {
  Result local;
  local.bytes_received = bytes_received;
  local.resolved_url = attempt_url;

  char *effective_url = nullptr;
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
  if (effective_url && *effective_url) {
    local.resolved_url = effective_url;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &local.http_code);

  if (code != CURLE_OK) {
    local.error = std::string("download failed: ") + curl_easy_strerror(code);
    if (result) {
      *result = std::move(local);
    }
    return false;
  }

  if (RequiresHttpStatusCheck(local.resolved_url) && local.http_code != 200) {
    local.error = "download failed with HTTP " + std::to_string(local.http_code);
    if (result) {
      *result = std::move(local);
    }
    return false;
  }

  local.ok = true;
  if (result) {
    *result = std::move(local);
  }
  return true;
}

template <typename PrepareFn, typename CleanupFn>
bool DownloadWithFallback(const std::vector<std::string> &urls,
                          const Options &options, PrepareFn prepare,
                          CleanupFn cleanup, Result *result) {
  Result last_result;
  for (const auto &url : urls) {
    if (url.empty()) {
      continue;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
      last_result.error = "failed to initialize libcurl";
      continue;
    }

    ProgressState progress_state{options.progress_cb};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                     options.follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, options.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallbackFn);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_state);

    size_t bytes_received = 0;
    if (!prepare(curl, &bytes_received, &last_result)) {
      curl_easy_cleanup(curl);
      cleanup();
      continue;
    }

    CURLcode code = curl_easy_perform(curl);
    const bool ok =
        FinalizeResult(url, curl, code, bytes_received, &last_result);
    curl_easy_cleanup(curl);
    if (ok) {
      if (result) {
        *result = last_result;
      }
      return true;
    }

    cleanup();
  }

  if (result) {
    *result = std::move(last_result);
  }
  return false;
}

} // namespace

bool DownloadText(const std::vector<std::string> &urls, const Options &options,
                  std::string *content, Result *result) {
  if (content) {
    content->clear();
  }

  MemoryBuffer buffer;
  buffer.max_size = options.max_bytes;
  const bool ok = DownloadWithFallback(
      urls, options,
      [&buffer](CURL *curl, size_t *bytes_received, Result *result_state) {
        buffer.data.clear();
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToMemory);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        *bytes_received = 0;
        if (result_state) {
          result_state->error.clear();
        }
        return true;
      },
      []() {}, result);

  if (!ok) {
    return false;
  }

  if (content) {
    *content = std::move(buffer.data);
  }
  if (result) {
    result->bytes_received = content ? content->size() : buffer.data.size();
  }
  return true;
}

bool DownloadFile(const std::vector<std::string> &urls,
                  const std::filesystem::path &dest, const Options &options,
                  Result *result) {
  std::ofstream out;
  FileWriter writer;

  const auto cleanup = [&]() {
    if (out.is_open()) {
      out.close();
    }
    std::error_code ec;
    std::filesystem::remove(dest, ec);
  };

  const bool ok = DownloadWithFallback(
      urls, options,
      [&](CURL *curl, size_t *bytes_received, Result *result_state) {
        cleanup();
        std::string err;
        if (!vinput::file::EnsureParentDirectory(dest, &err)) {
          if (result_state) {
            result_state->error = err;
          }
          return false;
        }
        out.open(dest, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
          if (result_state) {
            result_state->error =
                "failed to open destination file: " + dest.string();
          }
          return false;
        }
        writer.out = &out;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
        *bytes_received = 0;
        return true;
      },
      cleanup, result);

  if (out.is_open()) {
    out.close();
  }

  return ok;
}

} // namespace vinput::download
