#include "post_processor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/llm/defaults.h"
#include "common/net/http_client.h"
#include "common/net/http_types.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/debug_log.h"
#include "common/utils/interpolation.h"
#include "common/utils/path_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/url_utils.h"

#include "daemon/postprocess/prompt_template.h"

namespace {

using json = nlohmann::json;
constexpr std::size_t kMaxLoggedResponseBytes = 2048;
constexpr std::size_t kMaxResponseBytes = 1ULL * 1024ULL * 1024ULL; // 1 MB limit
constexpr long kDefaultConnectTimeoutMs = 5000;

json BuildCandidatesResponseFormat() {
  return {
      {"type", "json_object"},
  };
}

std::string BuildRequestUrl(const std::string& base_url) {
  if (base_url.empty()) {
    return {};
  }

  constexpr std::string_view kChatCompletions = vinput::llm::kOpenAiChatCompletionsPath;
  if (std::string_view(base_url).ends_with(kChatCompletions)) {
    return base_url;
  }

  return vinput::url::JoinPath(base_url, kChatCompletions);
}

std::string QuoteForLog(std::string_view text) {
  if (text.size() > kMaxLoggedResponseBytes) {
    std::string truncated(text.substr(0, kMaxLoggedResponseBytes));
    truncated += "...(truncated)";
    return truncated;
  }
  return std::string(text);
}

void LogResponseSummary(const LlmProvider& provider, const std::string& url, long status_code,
                        double total_time_ms) {
  vinput::debug::Log("LLM request provider=%s url=%s status=%ld time=%.1fms\n",
                     provider.id.empty() ? "(unnamed)" : provider.id.c_str(), url.c_str(),
                     status_code, total_time_ms);
}

void LogLlmInput(const LlmProvider& provider, const std::string& url, std::string_view text) {
  vinput::debug::Log("LLM input provider=%s url=%s: %s\n",
                     provider.id.empty() ? "(unnamed)" : provider.id.c_str(), url.c_str(),
                     QuoteForLog(text).c_str());
}

void LogLlmRequest(const LlmProvider& provider, const std::string& url,
                   const std::vector<vinput::net::HttpHeader>& headers, std::string_view body) {
  // Debug-only: dump headers and body verbatim. The API key is included in
  // plain text since this is gated by VINPUT_DEBUG and intended for local
  // inspection; users who share logs are responsible for redacting first.
  std::string header_dump;
  for (const auto& header : headers) {
    if (!header_dump.empty()) {
      header_dump.append("; ");
    }
    header_dump.append(header.name);
    header_dump.append(": ");
    header_dump.append(header.value);
  }
  vinput::debug::Log("LLM request provider=%s url=%s headers=[%s]\n",
                     provider.id.empty() ? "(unnamed)" : provider.id.c_str(), url.c_str(),
                     header_dump.c_str());
  vinput::debug::Log("LLM request body: %s\n", QuoteForLog(body).c_str());
}

void LogResponseBody(const char* prefix, const std::string& url, std::string_view body) {
  vinput::debug::Log("%s %s: %s\n", prefix, url.c_str(), QuoteForLog(body).c_str());
}

std::vector<std::string> ExtractCandidates(const json& response) {
  const auto choices_it = response.find("choices");
  if (choices_it == response.end() || !choices_it->is_array() || choices_it->empty()) {
    return {};
  }

  const auto& choice = (*choices_it)[0];
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
  } catch (const std::exception&) {
    return {};
  }

  const auto candidates_it = content_json.find("candidates");
  if (candidates_it == content_json.end() || !candidates_it->is_array()) {
    return {};
  }

  std::vector<std::string> candidates;
  for (const auto& value : *candidates_it) {
    if (!value.is_string()) {
      continue;
    }
    const auto candidate_text = value.get<std::string>();
    const auto trimmed_candidate = vinput::str::TrimAsciiWhitespaceView(candidate_text);
    if (!trimmed_candidate.empty()) {
      candidates.emplace_back(trimmed_candidate);
    }
  }

  return candidates;
}

constexpr std::string_view kXmlTagAsr = "vinput-asr";
constexpr std::string_view kXmlTagSelected = "vinput-selected";
constexpr std::string_view kXmlTagContext = "vinput-context";

std::string WrapXmlBlock(std::string_view tag, std::string_view text) {
  std::string out;
  out.reserve(tag.size() * 2 + text.size() + 12);
  out.push_back('<');
  out.append(tag);
  out.append(">\n");
  out.append(text);
  if (text.empty() || text.back() != '\n') {
    out.push_back('\n');
  }
  out.append("</");
  out.append(tag);
  out.push_back('>');
  return out;
}

std::string BuildContextPrefix(int max_lines) {
  if (max_lines <= 0) {
    return {};
  }
  const auto path = vinput::path::ContextCachePath();
  std::ifstream ifs(path);
  if (!ifs) {
    return {};
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(ifs, line)) {
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  if (lines.empty()) {
    return {};
  }
  const auto start =
      (static_cast<int>(lines.size()) > max_lines) ? lines.end() - max_lines : lines.begin();
  std::string result = "Recent input history (use to fix ASR errors):\n";
  for (auto it = start; it != lines.end(); ++it) {
    result += *it;
    result += '\n';
  }
  result += '\n';
  return result;
}

std::string BuildConstraintsSuffix(int max_llm_candidates) {
  if (max_llm_candidates <= 0) {
    return {};
  }
  char buf[768];
  std::snprintf(buf, sizeof(buf),
                "\n\n## Constraints\n"
                "- Return only the JSON object described below.\n"
                "- Each candidate must contain only the final rewritten text.\n"
                "- Do not include explanations, Markdown fences, or extra keys.\n"
                "\n\n## Format\n"
                "Return up to %d distinct candidate(s) in a JSON object:\n"
                "```json\n"
                "{\"candidates\": [\"<string>\"]}\n"
                "```",
                max_llm_candidates);
  return buf;
}

// Merge user-supplied extra_body fields into the outgoing request. Some
// top-level fields are load-bearing for the candidates contract (messages
// schema, non-streaming response, json_object response_format) and are refused
// to prevent the merge from breaking response parsing. Everything else passes
// through so provider-specific knobs like enable_thinking / reasoning /
// thinking / top_p work without a whitelist.
void MergeExtraBody(json& request, const nlohmann::json& extra, const LlmProvider& provider) {
  if (!extra.is_object() || extra.empty()) {
    return;
  }
  static constexpr std::array<std::string_view, 3> kProtected = {"messages", "stream",
                                                                 "response_format"};
  for (auto it = extra.begin(); it != extra.end(); ++it) {
    const std::string& key = it.key();
    if (std::find(kProtected.begin(), kProtected.end(), key) != kProtected.end()) {
      vinput::debug::Log("LLM extra_body provider=%s: ignoring protected key '%s'\n",
                         provider.id.empty() ? "(unnamed)" : provider.id.c_str(), key.c_str());
      continue;
    }
    request[key] = it.value();
  }
}

std::optional<std::vector<std::string>>
RewriteWithOpenAiCompatible(const std::string& text, const vinput::scene::Definition& scene,
                            const LlmProvider& provider, int max_llm_candidates,
                            std::string* error_out, const std::string& task_prompt = {},
                            const std::atomic<bool>* shutdown_flag = nullptr,
                            const std::atomic<bool>* request_cancel_flag = nullptr,
                            std::string_view asr_var = {}, std::string_view selected_var = {}) {
  const auto is_cancelled = [&]() noexcept {
    return (shutdown_flag != nullptr && shutdown_flag->load(std::memory_order_relaxed)) ||
           (request_cancel_flag != nullptr && request_cancel_flag->load(std::memory_order_relaxed));
  };

  if (is_cancelled()) {
    if (error_out != nullptr) {
      error_out->clear();
    }
    return std::nullopt;
  }

  const std::string url = BuildRequestUrl(provider.base_url);
  if (url.empty()) {
    return std::nullopt;
  }

  // For the regular path, scene.prompt may still be a `file:///` reference and
  // needs resolution here. For the command path the caller (ProcessCommand)
  // has already resolved command_scene.prompt before deciding whether to
  // splice in the ASR command, so task_prompt is always content.
  std::string base_prompt = task_prompt.empty() ? scene.prompt : task_prompt;
  if (task_prompt.empty() && vinput::prompt_template::IsFileUri(base_prompt)) {
    std::string load_err;
    auto loaded = vinput::prompt_template::LoadFromFileUri(base_prompt, &load_err);
    if (!loaded) {
      if (error_out != nullptr) {
        *error_out = "Prompt file load failed: " + base_prompt + ": " + load_err;
      }
      return std::nullopt;
    }
    base_prompt = std::move(*loaded);
  }

  const std::string context_prefix = BuildContextPrefix(scene.context_lines);
  const std::string constraints_suffix = BuildConstraintsSuffix(max_llm_candidates);

  // We send a single user message. The output-format constraints are appended
  // verbatim at the end regardless of interpolation, since downstream parsing
  // depends on that contract.
  std::string user_content;
  if (vinput::prompt_template::HasInterpolation(base_prompt)) {
    // Author owns the layout: `{{asr}}`, `{{selected}}` and `{{context}}`
    // get substituted in-place; no auto-append of context or data text.
    vinput::prompt_template::Vars vars{
        .asr = asr_var.empty() ? std::string_view(text) : asr_var,
        .selected = selected_var,
        .context = context_prefix,
    };
    user_content = vinput::prompt_template::Interpolate(base_prompt, vars);
  } else {
    // No interpolation — wrap data in Vinput-scoped XML tags so prompt
    // instructions and user-provided data remain clearly separated.
    user_content = std::move(base_prompt);
    if (!user_content.empty() && user_content.back() != '\n') {
      user_content.append("\n\n");
    } else if (!user_content.empty()) {
      user_content.push_back('\n');
    }
    if (!context_prefix.empty()) {
      user_content.append(WrapXmlBlock(kXmlTagContext, context_prefix));
      user_content.push_back('\n');
    }
    if (!text.empty()) {
      user_content.append(WrapXmlBlock(kXmlTagAsr, text));
      user_content.push_back('\n');
    }
  }
  user_content.append(constraints_suffix);

  if (vinput::debug::Enabled()) {
    LogLlmInput(provider, url, text);
  }

  std::vector<json> messages;
  messages.push_back({{"role", "user"}, {"content", user_content}});

  json request = {
      {"model", scene.model},
      {"stream", false},
      {"temperature", 0.2},
      {"response_format", BuildCandidatesResponseFormat()},
      {"messages", std::move(messages)},
  };
  MergeExtraBody(request, provider.extra_body, provider);
  const std::string request_body = request.dump();

  const std::string resolved_api_key = vinput::interpolate::InterpolateEnv(provider.api_key);
  if (vinput::interpolate::HasInterpolation(provider.api_key) && resolved_api_key.empty()) {
    fprintf(stderr,
            "vinput-daemon: environment variable in api_key ('%s') for provider '%s' is unset or "
            "empty\n",
            provider.api_key.c_str(), provider.id.c_str());
  }

  std::atomic<bool> cancel_token{is_cancelled()};

  vinput::net::HttpRequest req;
  req.url = url;
  req.method = vinput::net::HttpMethod::Post;
  req.body = request_body;
  req.timeout_ms = scene.timeout_ms;
  req.connect_timeout_ms =
      static_cast<long>(std::min(scene.timeout_ms, static_cast<int>(kDefaultConnectTimeoutMs)));
  req.max_response_bytes = kMaxResponseBytes;
  req.cancel_token = &cancel_token;
  req.allow_auto_resend = false;
  req.follow_redirects = false;
  req.headers.push_back({"Content-Type", "application/json"});
  req.headers.push_back({"User-Agent", std::string(vinput::llm::kHttpUserAgent)});
  if (!resolved_api_key.empty()) {
    req.headers.push_back({std::string(vinput::llm::kAuthorizationHeader),
                           std::string(vinput::llm::kBearerPrefix) + resolved_api_key});
  }

  req.progress_cb = [&](const vinput::net::HttpProgress&) {
    if (is_cancelled()) {
      cancel_token.store(true, std::memory_order_relaxed);
    }
  };

  if (vinput::debug::Enabled()) {
    LogLlmRequest(provider, url, req.headers, request_body);
  }

  const auto res = vinput::net::HttpClient::Instance().Request(req);

  if (res.cancelled) {
    vinput::debug::Log("LLM request provider=%s url=%s cancelled after %.1fms\n",
                       provider.id.empty() ? "(unnamed)" : provider.id.c_str(), url.c_str(),
                       res.total_time_ms);
    if (error_out != nullptr) {
      error_out->clear();
    }
    return std::nullopt;
  }

  if (!res.error_message.empty()) {
    const std::string msg = std::string("LLM request failed: ") + res.error_message;
    fprintf(stderr, "vinput-daemon: LLM request provider=%s url=%s failed after %.1fms: %s\n",
            provider.id.empty() ? "(unnamed)" : provider.id.c_str(), url.c_str(), res.total_time_ms,
            res.error_message.c_str());
    if (error_out != nullptr) {
      *error_out = msg;
    }
    return std::nullopt;
  }

  LogResponseSummary(provider, url, res.status_code, res.total_time_ms);

  if (res.status_code < 200 || res.status_code >= 300) {
    const std::string msg = "HTTP " + std::to_string(res.status_code) + ": " + res.body;
    if (vinput::debug::Enabled()) {
      LogResponseBody("LLM error response from", url, res.body);
    }
    if (error_out != nullptr) {
      *error_out = msg;
    }
    return std::nullopt;
  }

  if (vinput::debug::Enabled()) {
    LogResponseBody("LLM raw response from", url, res.body);
  }

  json response;
  try {
    response = json::parse(res.body);
  } catch (const std::exception& e) {
    fprintf(stderr, "vinput-daemon: failed to parse LLM response JSON from %s: %s\n", url.c_str(),
            e.what());
    return std::nullopt;
  }

  const auto error_it = response.find("error");
  if (error_it != response.end()) {
    fprintf(stderr, "vinput-daemon: LLM response from %s contains error\n", url.c_str());
    if (vinput::debug::Enabled()) {
      LogResponseBody("LLM parsed error payload from", url, error_it->dump());
    }
    return std::nullopt;
  }

  auto candidates = ExtractCandidates(response);
  if (candidates.empty()) {
    fprintf(stderr, "vinput-daemon: LLM response from %s returned no valid candidates\n",
            url.c_str());
    return std::nullopt;
  }

  return candidates;
}

void AppendCandidate(vinput::result::Payload& payload, std::string text, const char* source) {
  text = vinput::str::TrimAsciiWhitespace(text);
  if (text.empty()) {
    return;
  }

  payload.candidates.push_back(
      vinput::result::Candidate{.text = std::move(text), .source = source});
}

} // namespace

void PostProcessor::Shutdown() {
  shutting_down_.store(true, std::memory_order_relaxed);
}

vinput::result::Payload PostProcessor::Process(const std::string& raw_text,
                                               const vinput::scene::Definition& scene,
                                               const CoreConfig& settings, std::string* error_out,
                                               const std::atomic<bool>* cancel_flag) const {
  const std::string normalized = vinput::str::TrimAsciiWhitespace(raw_text);
  if (normalized.empty()) {
    return {};
  }

  const int max_llm_candidates = vinput::scene::NormalizeLlmMaxCandidates(scene.llm_max_candidates);

  vinput::result::Payload fallback;
  AppendCandidate(fallback, normalized, vinput::result::kSourceRaw);
  fallback.commitText = normalized;

  const LlmProvider* provider = ResolveLlmProvider(settings, scene.provider_id);
  if (provider == nullptr || max_llm_candidates == 0) {
    return fallback;
  }

  auto rewritten = RewriteWithOpenAiCompatible(normalized, scene, *provider, max_llm_candidates,
                                               error_out, {}, &shutting_down_, cancel_flag);
  if (!rewritten.has_value()) {
    return fallback;
  }

  vinput::result::Payload payload;
  std::set<std::string> seen;

  if (scene.raw_cand) {
    AppendCandidate(payload, normalized, vinput::result::kSourceRaw);
    seen.insert(normalized);
  }

  std::string first_llm_text;
  int accepted_llm_candidates = 0;
  for (auto& text : *rewritten) {
    std::string trimmed = vinput::str::TrimAsciiWhitespace(text);
    if (trimmed.empty()) {
      continue;
    }
    if (seen.insert(trimmed).second) {
      if (first_llm_text.empty()) {
        first_llm_text = trimmed;
      }
      AppendCandidate(payload, std::move(trimmed), vinput::result::kSourceLlm);
      ++accepted_llm_candidates;
      if (accepted_llm_candidates >= max_llm_candidates) {
        break;
      }
    }
  }

  if (payload.candidates.empty()) {
    return fallback;
  }

  payload.commitText = first_llm_text.empty() ? normalized : first_llm_text;
  return payload;
}

vinput::result::Payload
PostProcessor::ProcessCommand(const std::string& asr_text, const std::string& selected_text,
                              const vinput::scene::Definition& command_scene,
                              const CoreConfig& settings, std::string* error_out,
                              const std::atomic<bool>* cancel_flag) const {
  const std::string normalized_asr = vinput::str::TrimAsciiWhitespace(asr_text);

  vinput::result::Payload fallback;
  const std::string& fallback_text = normalized_asr.empty() ? selected_text : normalized_asr;
  AppendCandidate(fallback, fallback_text, vinput::result::kSourceRaw);
  fallback.commitText = fallback_text;

  if (normalized_asr.empty() || selected_text.empty()) {
    return fallback;
  }

  const int command_max_llm_candidates =
      vinput::scene::NormalizeLlmMaxCandidates(command_scene.llm_max_candidates);

  const LlmProvider* provider = ResolveLlmProvider(settings, command_scene.provider_id);
  if (provider == nullptr || command_max_llm_candidates == 0) {
    return fallback;
  }

  // Resolve any `file:///` reference up front so the URI is not
  // accidentally concatenated with data text.
  std::string task_prompt = command_scene.prompt;
  if (vinput::prompt_template::IsFileUri(task_prompt)) {
    std::string load_err;
    auto loaded = vinput::prompt_template::LoadFromFileUri(task_prompt, &load_err);
    if (!loaded) {
      if (error_out != nullptr) {
        *error_out = "Prompt file load failed: " + task_prompt + ": " + load_err;
      }
      return fallback;
    }
    task_prompt = std::move(*loaded);
  }

  // Interpolation mode ({{asr}}/{{selected}}/{{context}}): author owns layout.
  // Legacy mode: wrap ASR command and selected text in Vinput-scoped XML tags
  // so data is clearly separated from prompt instructions.
  const bool use_template = vinput::prompt_template::HasInterpolation(task_prompt);
  std::string_view asr_var;
  std::string_view selected_var;
  if (use_template) {
    asr_var = normalized_asr;
    selected_var = selected_text;
  } else {
    // Legacy mode: wrap data in Vinput-scoped XML tags.
    if (!task_prompt.empty() && task_prompt.back() != '\n') {
      task_prompt.push_back('\n');
    }
    task_prompt += '\n';
    task_prompt += WrapXmlBlock(kXmlTagAsr, normalized_asr);
    task_prompt += "\n\n";
    task_prompt += WrapXmlBlock(kXmlTagSelected, selected_text);
    task_prompt += '\n';
  }

  // Data is already embedded in task_prompt (legacy XML tags) or will be
  // substituted via {{asr}}/{{selected}} (interpolation). Pass empty text to
  // avoid double-appending.
  auto rewritten = RewriteWithOpenAiCompatible("", command_scene, *provider,
                                               command_max_llm_candidates, error_out, task_prompt,
                                               &shutting_down_, cancel_flag, asr_var, selected_var);

  vinput::result::Payload payload;
  std::set<std::string> seen;

  // 1st: original selected text (always)
  AppendCandidate(payload, selected_text, vinput::result::kSourceRaw);
  seen.insert(selected_text);

  // 2nd: ASR recognized command (always, if different from selected_text)
  if (seen.insert(normalized_asr).second) {
    AppendCandidate(payload, normalized_asr, vinput::result::kSourceAsr);
  }

  // 3rd+: LLM results (if available, deduplicated and capped)
  std::string first_llm_text;
  int accepted_llm_candidates = 0;
  if (rewritten.has_value()) {
    for (auto& text : *rewritten) {
      std::string trimmed = vinput::str::TrimAsciiWhitespace(text);
      if (trimmed.empty()) {
        continue;
      }
      if (seen.insert(trimmed).second) {
        if (first_llm_text.empty()) {
          first_llm_text = trimmed;
        }
        AppendCandidate(payload, std::move(trimmed), vinput::result::kSourceLlm);
        ++accepted_llm_candidates;
        if (accepted_llm_candidates >= command_max_llm_candidates) {
          break;
        }
      }
    }
  }

  payload.commitText = first_llm_text.empty() ? selected_text : first_llm_text;
  return payload;
}
