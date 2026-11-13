#pragma once

#include <string>

#include "common/llm/defaults.h"
#include "common/net/http_types.h"

namespace vinput::llm {

// Adds the headers every LLM provider request needs: a stable User-Agent, plus
// bearer authorization when the caller resolved an API key. Callers that share
// this path (CLI probing and daemon inference) must not drift on header shape.
inline void AppendProviderHeaders(vinput::net::HttpRequest* request,
                                  const std::string& resolved_api_key) {
  request->headers.push_back({.name = "User-Agent", .value = std::string(kHttpUserAgent)});
  if (resolved_api_key.empty()) {
    return;
  }
  request->headers.push_back({.name = std::string(kAuthorizationHeader),
                              .value = std::string(kBearerPrefix) + resolved_api_key});
}

} // namespace vinput::llm
