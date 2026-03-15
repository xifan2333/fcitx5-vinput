#include "post_processor.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <optional>

#include <set>
#include <string>

namespace {

using json = nlohmann::json;
constexpr std::size_t kMaxLoggedResponseBytes = 2048;
constexpr std::size_t kMaxResponseBytes = 1 * 1024 * 1024; // 1 MB limit

struct CurlGuard {
  CURL *curl = nullptr;
  struct curl_slist *headers = nullptr;

  ~CurlGuard() {
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
  }
};

size_t WriteResponseCallback(char *ptr, size_t size, size_t nmemb,
                             void *userdata) {
  const size_t total = size * nmemb;
  if (!userdata || !ptr || total == 0) {
    return 0;
  }

  auto *response = static_cast<std::string *>(userdata);
  if (response->size() + total > kMaxResponseBytes)
    return 0;
  response->append(ptr, total);
  return total;
}

std::string TrimAsciiWhitespace(std::string text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }

  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

int NormalizePostprocessCandidateCount(int n) {
  return n <= 0 ? 0 : n;
}

int NormalizeCommandCandidateCount(int n) {
  return n <= 0 ? 1 : n;
}

json BuildCandidatesSchema(int candidate_count) {
  json array_schema = {
      {"type", "array"},
      {"items", {{"type", "string"}}},
      {"minItems", 1},
      {"maxItems", candidate_count},
  };
  json schema = {
      {"type", "object"},
      {"properties", {{"candidates", array_schema}}},
      {"required", json::array({"candidates"})},
      {"additionalProperties", false},
  };
  return {
      {"type", "json_schema"},
      {"json_schema", {
          {"name", "candidates_response"},
          {"strict", true},
          {"schema", schema},
      }},
  };
}

std::string BuildRequestUrl(const std::string &base_url) {
  if (base_url.empty()) {
    return {};
  }

  constexpr std::string_view kChatCompletions = "/chat/completions";
  if (base_url.size() >= kChatCompletions.size() &&
      base_url.compare(base_url.size() - kChatCompletions.size(),
                       kChatCompletions.size(), kChatCompletions) == 0) {
    return base_url;
  }

  std::string url = base_url;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  url += kChatCompletions;
  return url;
}

std::string QuoteForLog(std::string_view text) {
  if (text.size() > kMaxLoggedResponseBytes) {
    std::string truncated(text.substr(0, kMaxLoggedResponseBytes));
    truncated += "...(truncated)";
    return truncated;
  }
  return std::string(text);
}

void LogResponseBody(const char *prefix, const std::string &url,
                     std::string_view body) {
  fprintf(stderr, "vinput-daemon: %s %s: %s\n", prefix, url.c_str(),
          QuoteForLog(body).c_str());
}

std::vector<std::string> ExtractCandidates(const json &response) {
  const auto choices_it = response.find("choices");
  if (choices_it == response.end() || !choices_it->is_array() ||
      choices_it->empty()) {
    return {};
  }

  const auto &choice = (*choices_it)[0];
  const auto message_it = choice.find("message");
  if (message_it == choice.end() || !message_it->is_object()) {
    return {};
  }

  const auto content_it = message_it->find("content");
  if (content_it == message_it->end() || !content_it->is_string()) {
    return {};
  }

  json content_json;
  try {
    content_json = json::parse(content_it->get<std::string>());
  } catch (const std::exception &) {
    return {};
  }

  const auto candidates_it = content_json.find("candidates");
  if (candidates_it == content_json.end() || !candidates_it->is_array()) {
    return {};
  }

  std::vector<std::string> candidates;
  for (const auto &value : *candidates_it) {
    if (!value.is_string()) {
      continue;
    }
    auto candidate = TrimAsciiWhitespace(value.get<std::string>());
    if (!candidate.empty()) {
      candidates.push_back(std::move(candidate));
    }
  }

  return candidates;
}

std::optional<std::vector<std::string>>
RewriteWithOpenAiCompatible(const std::string &text,
                            const vinput::scene::Definition &scene,
                            const CoreConfig &settings, int candidate_count,
                            std::string *error_out) {
  if (!settings.llm.enabled) {
    return std::nullopt;
  }
  const LlmProvider *provider = ResolveActiveLlmProvider(settings);
  if (!provider) {
    fprintf(stderr,
            "vinput-daemon: LLM enabled but no active provider configured\n");
    return std::nullopt;
  }

  if (scene.prompt.empty()) {
    return std::nullopt;
  }

  CurlGuard guard;
  guard.curl = curl_easy_init();
  if (!guard.curl) {
    fprintf(stderr, "vinput-daemon: failed to initialize libcurl\n");
    return std::nullopt;
  }

  const std::string url = BuildRequestUrl(provider->base_url);
  if (url.empty()) {
    return std::nullopt;
  }

  std::string system_prompt = scene.prompt;
  if (candidate_count > 1) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\nProvide exactly %d alternative versions.",
                  candidate_count);
    system_prompt += buf;
  }

  std::vector<json> messages = {
      {{"role", "system"}, {"content", system_prompt}},
  };
  messages.push_back({{"role", "user"}, {"content", text}});

  json request = {
      {"model", provider->model},
      {"stream", false},
      {"temperature", 0.2},
      {"response_format", BuildCandidatesSchema(candidate_count)},
      {"messages", std::move(messages)},
  };
  const std::string request_body = request.dump();

  guard.headers = curl_slist_append(nullptr, "Content-Type: application/json");
  if (!provider->api_key.empty()) {
    const std::string auth = "Authorization: Bearer " + provider->api_key;
    guard.headers = curl_slist_append(guard.headers, auth.c_str());
  }

  curl_easy_setopt(guard.curl, CURLOPT_POST, 1L);
  curl_easy_setopt(guard.curl, CURLOPT_HTTPHEADER, guard.headers);
  curl_easy_setopt(guard.curl, CURLOPT_POSTFIELDS, request_body.c_str());
  curl_easy_setopt(guard.curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(request_body.size()));
  curl_easy_setopt(guard.curl, CURLOPT_WRITEFUNCTION, WriteResponseCallback);
  curl_easy_setopt(guard.curl, CURLOPT_TIMEOUT_MS, provider->timeout_ms);
  curl_easy_setopt(guard.curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(guard.curl, CURLOPT_USERAGENT, "fcitx5-vinput/0.1");

  std::string response_body;
  curl_easy_setopt(guard.curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(guard.curl, CURLOPT_WRITEDATA, &response_body);

  CURLcode curl_code = curl_easy_perform(guard.curl);
  long status_code = 0;
  curl_easy_getinfo(guard.curl, CURLINFO_RESPONSE_CODE, &status_code);

  if (curl_code != CURLE_OK) {
    const std::string msg =
        std::string("LLM request failed: ") + curl_easy_strerror(curl_code);
    fprintf(stderr, "vinput-daemon: LLM request to %s failed: %s\n",
            url.c_str(), curl_easy_strerror(curl_code));
    if (error_out) *error_out = msg;
    return std::nullopt;
  }

  if (status_code < 200 || status_code >= 300) {
    const std::string msg =
        "HTTP " + std::to_string(status_code) + ": " + response_body;
    fprintf(stderr, "vinput-daemon: LLM request to %s returned HTTP %ld: %s\n",
            url.c_str(), status_code, response_body.c_str());
    if (error_out) *error_out = msg;
    return std::nullopt;
  }

  LogResponseBody("LLM raw response from", url, response_body);

  json response;
  try {
    response = json::parse(response_body);
  } catch (const std::exception &e) {
    fprintf(stderr,
            "vinput-daemon: failed to parse LLM response JSON from %s: %s\n",
            url.c_str(), e.what());
    return std::nullopt;
  }

  const auto error_it = response.find("error");
  if (error_it != response.end()) {
    fprintf(stderr, "vinput-daemon: LLM response from %s contains error: %s\n",
            url.c_str(), error_it->dump().c_str());
    return std::nullopt;
  }

  auto candidates = ExtractCandidates(response);
  if (candidates.empty()) {
    fprintf(stderr,
            "vinput-daemon: LLM response from %s returned no valid "
            "candidates\n",
            url.c_str());
    return std::nullopt;
  }

  return candidates;
}

void AppendUniqueCandidate(vinput::result::Payload &payload,
                           std::set<std::string> &seen, std::string text,
                           const char *source) {
  text = TrimAsciiWhitespace(std::move(text));
  if (text.empty() || !seen.insert(text).second) {
    return;
  }

  payload.candidates.push_back(
      vinput::result::Candidate{.text = std::move(text), .source = source});
}

} // namespace

PostProcessor::PostProcessor() {
  const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (code != CURLE_OK) {
    fprintf(stderr, "vinput-daemon: curl_global_init failed: %s\n",
            curl_easy_strerror(code));
  }
}

PostProcessor::~PostProcessor() { curl_global_cleanup(); }

vinput::result::Payload
PostProcessor::Process(const std::string &raw_text,
                       const vinput::scene::Definition &scene,
                       const CoreConfig &settings,
                       std::string *error_out) const {
  std::string normalized = TrimAsciiWhitespace(raw_text);
  if (normalized.empty()) {
    return {};
  }

  const int candidate_count =
      NormalizePostprocessCandidateCount(settings.llm.postprocessCandidateCount);

  vinput::result::Payload fallback;
  std::set<std::string> fallback_seen;
  AppendUniqueCandidate(fallback, fallback_seen, normalized, vinput::result::kSourceRaw);
  fallback.commitText = normalized;

  if (!settings.llm.enabled || candidate_count == 0 || scene.prompt.empty()) {
    return fallback;
  }

  auto rewritten =
      RewriteWithOpenAiCompatible(normalized, scene, settings, candidate_count,
                                   error_out);
  if (!rewritten.has_value()) {
    return fallback;
  }

  vinput::result::Payload payload;
  std::set<std::string> seen;
  AppendUniqueCandidate(payload, seen, normalized, vinput::result::kSourceRaw);
  for (auto &text : *rewritten) {
    AppendUniqueCandidate(payload, seen, std::move(text),
                          vinput::result::kSourceLlm);
  }

  payload.commitText = normalized;
  for (const auto &candidate : payload.candidates) {
    if (candidate.source == vinput::result::kSourceLlm) {
      payload.commitText = candidate.text;
      break;
    }
  }

  return payload;
}

vinput::result::Payload
PostProcessor::ProcessCommand(const std::string &asr_text,
                              const std::string &selected_text,
                              const CoreConfig &settings,
                              std::string *error_out) const {
  std::string normalized_asr = TrimAsciiWhitespace(asr_text);

  vinput::result::Payload fallback;
  std::set<std::string> fallback_seen;
  const std::string &fallback_text =
      normalized_asr.empty() ? selected_text : normalized_asr;
  AppendUniqueCandidate(fallback, fallback_seen, fallback_text,
                        vinput::result::kSourceRaw);
  fallback.commitText = fallback_text;

  if (normalized_asr.empty() || selected_text.empty()) {
    return fallback;
  }

  vinput::scene::Definition synthetic_scene;
  synthetic_scene.prompt =
      "You are an AI assistant helping with text editing via voice input. "
      "The user has given a voice command to operate on the selected text. "
      "Note that the command is transcribed from voice input and may contain "
      "speech recognition errors — infer the most likely intended operation "
      "from context. Execute the inferred command on the user's text and "
      "output ONLY the result. Do not include markdown formatting or "
      "explanations.\n\nUser voice command (may contain recognition errors): " +
      normalized_asr;

  const int command_candidate_count =
      NormalizeCommandCandidateCount(settings.llm.commandCandidateCount);

  // Early exit if LLM is disabled or candidate count is 0
  if (!settings.llm.enabled || command_candidate_count == 0) {
    return fallback;
  }

  auto rewritten =
      RewriteWithOpenAiCompatible(selected_text, synthetic_scene, settings,
                                   command_candidate_count, error_out);

  vinput::result::Payload payload;
  std::set<std::string> seen;
  // 1st: original selected text (always)
  AppendUniqueCandidate(payload, seen, selected_text, vinput::result::kSourceRaw);
  // 2nd: ASR recognized command (always)
  AppendUniqueCandidate(payload, seen, normalized_asr, vinput::result::kSourceAsr);
  // 3rd+: LLM results (if available)
  if (rewritten.has_value()) {
    for (auto &text : *rewritten) {
      AppendUniqueCandidate(payload, seen, std::move(text), vinput::result::kSourceLlm);
    }
  }

  // commitText is the first LLM result
  payload.commitText = selected_text;
  for (const auto &c : payload.candidates) {
    if (c.source == vinput::result::kSourceLlm) {
      payload.commitText = c.text;
      break;
    }
  }

  return payload;
}
