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

std::string BuildJsonCandidateInstruction(int candidate_count) {
  return "Return exactly one JSON object with a top-level array field "
         "\"candidates\". The array should contain up to " +
         std::to_string(candidate_count) +
         " distinct rewritten results as plain strings. Do not include "
         "markdown, code fences, or explanations. "
         "Always respond with a JSON object even if there is only one result.";
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

bool LooksLikeHtml(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return false;
  }

  return text[begin] == '<';
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

std::optional<std::string> ExtractMessageContent(const json &choice) {
  const auto message_it = choice.find("message");
  if (message_it == choice.end() || !message_it->is_object()) {
    return std::nullopt;
  }

  const auto content_it = message_it->find("content");
  if (content_it == message_it->end()) {
    return std::nullopt;
  }

  if (content_it->is_string()) {
    return content_it->get<std::string>();
  }

  if (!content_it->is_array()) {
    return std::nullopt;
  }

  std::string combined;
  for (const auto &part : *content_it) {
    if (part.is_string()) {
      combined += part.get<std::string>();
      continue;
    }

    if (!part.is_object()) {
      continue;
    }

    const auto text_it = part.find("text");
    if (text_it != part.end() && text_it->is_string()) {
      combined += text_it->get<std::string>();
    }
  }

  if (combined.empty()) {
    return std::nullopt;
  }
  return combined;
}

std::vector<std::string> ExtractMessageContents(const json &response) {
  const auto choices_it = response.find("choices");
  if (choices_it == response.end() || !choices_it->is_array() ||
      choices_it->empty()) {
    return {};
  }

  std::vector<std::string> contents;
  for (const auto &choice : *choices_it) {
    auto content = ExtractMessageContent(choice);
    if (!content.has_value()) {
      continue;
    }

    auto rewritten = TrimAsciiWhitespace(std::move(*content));
    if (!rewritten.empty()) {
      contents.push_back(std::move(rewritten));
    }
  }

  return contents;
}

std::vector<std::string>
ExtractStructuredCandidates(std::string_view content_text) {
  json content_json;
  try {
    content_json = json::parse(content_text);
  } catch (const std::exception &) {
    return {};
  }

  if (!content_json.is_object()) {
    return {};
  }

  const auto candidates_it = content_json.find("candidates");
  if (candidates_it == content_json.end() || !candidates_it->is_array()) {
    return {};
  }

  std::vector<std::string> candidates;
  for (const auto &value : *candidates_it) {
    if (value.is_string()) {
      auto candidate = TrimAsciiWhitespace(value.get<std::string>());
      if (!candidate.empty()) {
        candidates.push_back(std::move(candidate));
      }
      continue;
    }

    if (!value.is_object()) {
      continue;
    }

    const auto text_it = value.find("text");
    if (text_it != value.end() && text_it->is_string()) {
      auto candidate = TrimAsciiWhitespace(text_it->get<std::string>());
      if (!candidate.empty()) {
        candidates.push_back(std::move(candidate));
      }
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

  CURL *curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "vinput-daemon: failed to initialize libcurl\n");
    return std::nullopt;
  }

  const std::string url = BuildRequestUrl(provider->base_url);
  if (url.empty()) {
    curl_easy_cleanup(curl);
    return std::nullopt;
  }

  std::vector<json> messages = {
      {{"role", "system"}, {"content", scene.prompt}},
      {{"role", "system"}, {"content", BuildJsonCandidateInstruction(candidate_count)}},
  };
  messages.push_back({{"role", "user"}, {"content", text}});

  json request = {
      {"model", provider->model},
      {"stream", false},
      {"temperature", 0.2},
      {"response_format", {{"type", "json_object"}}},
      {"messages", std::move(messages)},
  };
  const std::string request_body = request.dump();

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!provider->api_key.empty()) {
    const std::string auth = "Authorization: Bearer " + provider->api_key;
    headers = curl_slist_append(headers, auth.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(request_body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponseCallback);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, provider->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-vinput/0.1");

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

  CURLcode curl_code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

  if (curl_code != CURLE_OK) {
    const std::string msg = std::string("LLM request failed: ") + curl_easy_strerror(curl_code);
    fprintf(stderr, "vinput-daemon: LLM request to %s failed: %s\n",
            url.c_str(), curl_easy_strerror(curl_code));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (error_out) *error_out = msg;
    return std::nullopt;
  }

  if (status_code < 200 || status_code >= 300) {
    const std::string msg = "HTTP " + std::to_string(status_code) + ": " + response_body;
    fprintf(stderr, "vinput-daemon: LLM request to %s returned HTTP %ld: %s\n",
            url.c_str(), status_code, response_body.c_str());
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (error_out) *error_out = msg;
    return std::nullopt;
  }

  LogResponseBody("LLM raw response from", url, response_body);

  if (LooksLikeHtml(response_body)) {
    fprintf(stderr,
            "vinput-daemon: LLM endpoint %s returned HTML instead of "
            "JSON\n",
            url.c_str());
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return std::nullopt;
  }

  json response;
  try {
    response = json::parse(response_body);
  } catch (const std::exception &e) {
    fprintf(stderr,
            "vinput-daemon: failed to parse LLM response JSON from "
            "%s: %s\n",
            url.c_str(), e.what());
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return std::nullopt;
  }

  const auto error_it = response.find("error");
  if (error_it != response.end()) {
    fprintf(stderr, "vinput-daemon: LLM response from %s contains error: %s\n",
            url.c_str(), error_it->dump().c_str());
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return std::nullopt;
  }

  auto contents = ExtractMessageContents(response);
  if (contents.empty()) {
    fprintf(stderr,
            "vinput-daemon: LLM response from %s does not contain "
            "message content\n",
            url.c_str());
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return std::nullopt;
  }

  std::vector<std::string> structured_candidates;
  for (const auto &content : contents) {
    auto parsed = ExtractStructuredCandidates(content);
    structured_candidates.insert(structured_candidates.end(),
                                 std::make_move_iterator(parsed.begin()),
                                 std::make_move_iterator(parsed.end()));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (!structured_candidates.empty()) {
    return structured_candidates;
  }

  fprintf(stderr,
          "vinput-daemon: LLM response from %s did not contain a "
          "valid JSON candidates array, falling back to plain text\n",
          url.c_str());
  return contents;
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
  if (!settings.llm.enabled || candidate_count == 0 || scene.prompt.empty()) {
    return vinput::result::PlainTextPayload(normalized);
  }

  auto rewritten =
      RewriteWithOpenAiCompatible(normalized, scene, settings, candidate_count,
                                   error_out);
  if (!rewritten.has_value()) {
    return vinput::result::PlainTextPayload(normalized);
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
  if (normalized_asr.empty() || selected_text.empty()) {
    // Can't do anything if there's no command or no selected text
    return vinput::result::PlainTextPayload(
        normalized_asr.empty() ? selected_text : normalized_asr);
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
    return vinput::result::PlainTextPayload(normalized_asr);
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
