#pragma once
#include <string>
#include <string_view>

namespace vinput::url {

/// Join a base URL with a path, inserting exactly one '/' between them.
/// Path constants (e.g. vinput::llm::kOpenAiModelsPath) start with '/'.
inline std::string JoinPath(std::string_view base_url, std::string_view path) {
  std::string url(base_url);
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  url += path;
  return url;
}

} // namespace vinput::url
