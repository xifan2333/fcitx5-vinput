#include "post_processor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <fstream>
#include <glaze/core/common.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/config/core_config_types.h"
#include "common/config/core_config_view.h"
#include "common/i18n.h"
#include "common/json/json_access.h"
#include "common/llm/defaults.h"
#include "common/net/http_client.h"
#include "common/net/http_types.h"
#include "common/notify/notification_bridge.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/debug_log.h"
#include "common/utils/interpolation.h"
#include "common/utils/path_utils.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/url_utils.h"

#include "daemon/postprocess/candidate_parser.h"
#include "daemon/postprocess/prompt_template.h"

namespace {

constexpr std::size_t kMaxResponseBytes = 1ULL * 1024ULL * 1024ULL; // 1 MB limit
constexpr long kDefaultConnectTimeoutMs = 5000;

glz::json_t BuildCandidatesResponseFormat() {
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

// TODO(#407): Modernize LLM payload logging (request/response body and header dumps)
// after migrating to modern zero-copy JSON library (e.g. glaze).

void LogResponseSummary(const LlmProvider& provider, const std::string& url, long status_code,
                        double total_time_ms) {
  VLOG_DEBUG("LLM request provider={} url={} status={} time={:.1f}ms",
             provider.id.empty() ? "(unnamed)" : provider.id, url, status_code, total_time_ms);
}

std::vector<std::string> ExtractCandidates(const glz::json_t& response, int max_candidates) {
  const auto* choices = vinput::json::Find(response, "choices");
  if (choices == nullptr || !choices->is_array() || choices->get_array().empty()) {
    return {};
  }

  const auto* message = vinput::json::Find(choices->get_array().front(), "message");
  if (message == nullptr || !message->is_object()) {
    return {};
  }

  const auto* content = vinput::json::Find(*message, "content");
  if (content == nullptr || !content->is_string()) {
    return {};
  }

  return vinput::postprocess::ExtractCandidatesAuto(content->get<std::string>(), max_candidates);
}

constexpr std::string_view kXmlTagAsr = "vinput-asr";
constexpr std::string_view kXmlTagSelected = "vinput-selected";
constexpr std::string_view kXmlTagContext = "vinput-context";

std::string WrapXmlBlock(std::string_view tag, std::string_view text) {
  std::string out;
  out.reserve((tag.size() * 2) + text.size() + 12);
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
  const auto start = (std::cmp_greater(lines.size(), max_lines))
                         ? lines.end() - static_cast<std::ptrdiff_t>(max_lines)
                         : lines.begin();
  std::string result = "Recent input history (use to fix ASR errors):\n";
  for (auto it = start; it != lines.end(); ++it) {
    result += *it;
    result += '\n';
  }
  result += '\n';
  return result;
}

std::string BuildConstraintsSuffix(int max_llm_candidates, bool json_object = true) {
  if (max_llm_candidates <= 0) {
    return {};
  }
  if (!json_object) {
    if (max_llm_candidates == 1) {
      return "\n\n## Constraints\n"
             "- Return only the final rewritten text directly.\n"
             "- Do not include explanations, markdown fences, or extra commentary.";
    }
    return fmt::format(
        "\n\n## Constraints\n"
        "- Return up to {} distinct candidate(s), exactly one candidate per line.\n"
        "- Each line must contain only the final rewritten text.\n"
        "- Do not include numbering, explanations, markdown fences, or extra commentary.",
        max_llm_candidates);
  }
  return fmt::format("\n\n## Constraints\n"
                     "- Return only the JSON object described below.\n"
                     "- Each candidate must contain only the final rewritten text.\n"
                     "- Do not include explanations, Markdown fences, or extra keys.\n"
                     "\n\n## Format\n"
                     "Return up to {} distinct candidate(s) in a JSON object:\n"
                     "```json\n"
                     "{{\"candidates\": [\"<string>\"]}}\n"
                     "```",
                     max_llm_candidates);
}

// Merge user-supplied extra_body fields into the outgoing request. Some
// top-level fields are load-bearing for the candidates contract (messages
// schema, non-streaming response, json_object response_format) and are refused
// to prevent the merge from breaking response parsing. Everything else passes
// through so provider-specific knobs like enable_thinking / reasoning /
// thinking / top_p work without a whitelist.
void MergeExtraBody(glz::json_t& request, const glz::json_t& extra, const LlmProvider& provider) {
  if (!extra.is_object() || extra.empty()) {
    return;
  }
  static constexpr std::array<std::string_view, 3> kProtected = {"messages", "stream",
                                                                 "response_format"};
  for (const auto& [key, value] : extra.get_object()) {
    if (std::ranges::find(kProtected, key) != kProtected.end()) {
      VLOG_DEBUG("LLM extra_body provider={}: ignoring protected key '{}'",
                 provider.id.empty() ? "(unnamed)" : provider.id, key);
      continue;
    }
    request[key] = value;
  }
}

bool IsCancellationRequested(const std::atomic<bool>* shutdown_flag,
                             const std::atomic<bool>* request_cancel_flag) noexcept {
  return (shutdown_flag != nullptr && shutdown_flag->load(std::memory_order_relaxed)) ||
         (request_cancel_flag != nullptr && request_cancel_flag->load(std::memory_order_relaxed));
}

std::optional<std::string> ResolvePromptContent(std::string_view task_prompt,
                                                std::string_view scene_prompt,
                                                std::string* error_out) {
  std::string base_prompt =
      task_prompt.empty() ? std::string(scene_prompt) : std::string(task_prompt);
  if (task_prompt.empty() && vinput::prompt_template::IsFileUri(base_prompt)) {
    std::string load_err;
    auto loaded = vinput::prompt_template::LoadFromFileUri(base_prompt, &load_err);
    if (!loaded) {
      if (error_out != nullptr) {
        *error_out = fmt::sprintf(_("Failed to load prompt file '%s': %s"), base_prompt, load_err);
      }
      return std::nullopt;
    }
    return *loaded;
  }
  return base_prompt;
}

std::string BuildUserContent(std::string_view base_prompt, std::string_view text,
                             std::string_view context_prefix, std::string_view constraints_suffix,
                             std::string_view asr_var, std::string_view selected_var) {
  std::string user_content;
  if (vinput::prompt_template::HasInterpolation(base_prompt)) {
    const vinput::prompt_template::Vars vars{
        .asr = asr_var.empty() ? text : asr_var,
        .selected = selected_var,
        .context = context_prefix,
    };
    user_content = vinput::prompt_template::Interpolate(base_prompt, vars);
  } else {
    user_content = std::string(base_prompt);
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
  return user_content;
}

std::string BuildOpenAiRequestBody(const std::string& model, std::string user_content,
                                   const LlmProvider& provider) {
  glz::json_t::array_t messages;
  messages.emplace_back(glz::json_t{{"role", "user"}, {"content", std::move(user_content)}});

  glz::json_t request = {
      {"model", model},
      {"stream", false},
      {"temperature", 0.2},
      {"messages", glz::json_t(std::move(messages))},
  };
  if (provider.json_object) {
    request["response_format"] = BuildCandidatesResponseFormat();
  }
  MergeExtraBody(request, provider.extra_body, provider);
  return vinput::json::ToText(request);
}

std::optional<std::vector<std::string>>
ParseOpenAiChatResponse(const vinput::net::HttpResponse& res, const std::string& url,
                        int max_llm_candidates, std::string* error_out) {
  if (res.status_code < 200 || res.status_code >= 300) {
    const std::string msg = std::string(_("LLM request returned an HTTP error.")) + "\nHTTP " +
                            std::to_string(res.status_code) + ": " + res.body;
    if (error_out != nullptr) {
      *error_out = msg;
    }
    return std::nullopt;
  }

  glz::json_t response;
  if (const auto ec = glz::read_json(response, res.body); bool(ec)) {
    VLOG_ERROR("failed to parse LLM response JSON from {}: {}", url,
               glz::format_error(ec, res.body));
    return std::nullopt;
  }

  if (vinput::json::Find(response, "error") != nullptr) {
    VLOG_ERROR("LLM response from {} contains error", url);
    return std::nullopt;
  }

  auto candidates = ExtractCandidates(response, max_llm_candidates);
  if (candidates.empty()) {
    VLOG_WARN("LLM response from {} returned no valid candidates", url);
    return std::nullopt;
  }

  return candidates;
}

vinput::net::HttpRequest
BuildOpenAiHttpRequest(const std::string& url, std::string request_body,
                       const LlmProvider& provider, int timeout_ms, std::atomic<bool>* cancel_token,
                       std::function<void(const vinput::net::HttpProgress&)> progress_cb) {
  const std::string resolved_api_key = vinput::interpolate::InterpolateEnv(provider.api_key);
  if (vinput::interpolate::HasInterpolation(provider.api_key) && resolved_api_key.empty()) {
    VLOG_WARN("environment variable in api_key ('{}') for provider '{}' is unset or empty",
              provider.api_key, provider.id);
  }

  vinput::net::HttpRequest req;
  req.url = url;
  req.method = vinput::net::HttpMethod::Post;
  req.body = std::move(request_body);
  req.timeout_ms = timeout_ms;
  req.connect_timeout_ms =
      static_cast<long>(std::min(timeout_ms, static_cast<int>(kDefaultConnectTimeoutMs)));
  req.max_response_bytes = kMaxResponseBytes;
  req.cancel_token = cancel_token;
  req.allow_auto_resend = false;
  req.follow_redirects = false;
  req.headers.push_back({"Content-Type", "application/json"});
  req.headers.push_back({"User-Agent", std::string(vinput::llm::kHttpUserAgent)});
  if (!resolved_api_key.empty()) {
    req.headers.push_back({std::string(vinput::llm::kAuthorizationHeader),
                           std::string(vinput::llm::kBearerPrefix) + resolved_api_key});
  }
  req.progress_cb = std::move(progress_cb);
  return req;
}

bool ValidateHttpExecution(const vinput::net::HttpResponse& res, bool is_cancelled,
                           const LlmProvider& provider, const std::string& url,
                           std::string* error_out) {
  if (res.cancelled || is_cancelled) {
    VLOG_DEBUG("LLM request provider={} url={} cancelled after {:.1f}ms",
               provider.id.empty() ? "(unnamed)" : provider.id, url, res.total_time_ms);
    if (error_out != nullptr) {
      error_out->clear();
    }
    return false;
  }

  if (!res.error_message.empty()) {
    const std::string msg = std::string(_("LLM request failed.")) + "\n" + res.error_message;
    VLOG_ERROR("LLM request provider={} url={} failed after {:.1f}ms: {}",
               provider.id.empty() ? "(unnamed)" : provider.id, url, res.total_time_ms,
               res.error_message);
    if (error_out != nullptr) {
      *error_out = msg;
    }
    return false;
  }
  return true;
}

std::optional<std::vector<std::string>>
RewriteWithOpenAiCompatible(const std::string& text, const vinput::scene::Definition& scene,
                            const LlmProvider& provider, int max_llm_candidates,
                            std::string* error_out, const std::string& task_prompt = {},
                            const std::atomic<bool>* shutdown_flag = nullptr,
                            const std::atomic<bool>* request_cancel_flag = nullptr,
                            std::string_view asr_var = {}, std::string_view selected_var = {}) {
  const auto is_cancelled = [&]() noexcept {
    return IsCancellationRequested(shutdown_flag, request_cancel_flag);
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

  auto base_prompt = ResolvePromptContent(task_prompt, scene.prompt, error_out);
  if (!base_prompt) {
    return std::nullopt;
  }

  const std::string context_prefix = BuildContextPrefix(scene.context_lines);
  const std::string constraints_suffix =
      BuildConstraintsSuffix(max_llm_candidates, provider.json_object);
  const std::string user_content = BuildUserContent(*base_prompt, text, context_prefix,
                                                    constraints_suffix, asr_var, selected_var);
  std::string request_body = BuildOpenAiRequestBody(scene.model, user_content, provider);

  std::atomic<bool> cancel_token{is_cancelled()};
  auto req = BuildOpenAiHttpRequest(url, std::move(request_body), provider, scene.timeout_ms,
                                    &cancel_token, [&](const vinput::net::HttpProgress&) {
                                      if (is_cancelled()) {
                                        cancel_token.store(true, std::memory_order_relaxed);
                                      }
                                    });

  const auto res = vinput::net::HttpClient::Instance().Request(req);
  if (!ValidateHttpExecution(res, is_cancelled(), provider, url, error_out)) {
    return std::nullopt;
  }

  LogResponseSummary(provider, url, res.status_code, res.total_time_ms);
  return ParseOpenAiChatResponse(res, url, max_llm_candidates, error_out);
}

constexpr std::size_t kMaxBatchNotifications = 4;

void DrainAdapterStderr(std::string_view stderr_text, const LlmAdapter& adapter,
                        vinput::notify::NotificationBridge* notification_bridge) {
  if (stderr_text.empty() || notification_bridge == nullptr) {
    return;
  }
  std::size_t delivered = 0;
  vinput::process::DrainLinesFromBuffer(
      stderr_text,
      [notification_bridge, &adapter, &delivered](std::string_view line) -> bool {
        if (delivered >= kMaxBatchNotifications) {
          return false;
        }
        notification_bridge->DeliverLine(adapter.id, line);
        ++delivered;
        return true;
      },
      /*flush_tail=*/true);
}

bool ValidateAdapterProcessResult(const vinput::process::RunResult& result,
                                  const LlmAdapter& adapter, std::string* error_out) {
  if (result.launch_failed) {
    const std::string msg = fmt::sprintf(_("Failed to start adapter '%s'."), adapter.id);
    VLOG_ERROR("{}", msg);
    if (error_out != nullptr) {
      *error_out = msg;
    }
    return false;
  }

  if (result.timed_out) {
    const std::string msg = fmt::sprintf(_("Adapter '%s' timed out."), adapter.id);
    VLOG_ERROR("{}", msg);
    if (error_out != nullptr) {
      *error_out = msg;
    }
    return false;
  }

  if (result.exit_code != 0) {
    const std::string msg =
        fmt::sprintf(_("Adapter '%s' exited with code %d."), adapter.id, result.exit_code);
    VLOG_WARN("{}", msg);
    if (error_out != nullptr) {
      *error_out = msg;
    }
    return false;
  }
  return true;
}

std::optional<std::vector<std::string>>
RewriteWithCommandAdapter(std::string_view stdin_content, const vinput::scene::Definition& scene,
                          const LlmAdapter& adapter, int max_candidates,
                          vinput::notify::NotificationBridge* notification_bridge,
                          std::string* error_out, const std::atomic<bool>* shutdown_flag = nullptr,
                          const std::atomic<bool>* request_cancel_flag = nullptr) {
  if (IsCancellationRequested(shutdown_flag, request_cancel_flag)) {
    if (error_out != nullptr) {
      error_out->clear();
    }
    return std::nullopt;
  }

  if (adapter.command.empty()) {
    if (error_out != nullptr) {
      *error_out = fmt::sprintf(_("Adapter '%s' has no command configured."), adapter.id);
    }
    return std::nullopt;
  }

  vinput::process::CommandSpec spec;
  spec.command = adapter.command;
  spec.args = adapter.args;
  spec.env = adapter.env;
  spec.timeout_ms = scene.timeout_ms;

  const auto stdin_bytes = std::as_bytes(std::span(stdin_content));
  auto result = vinput::process::Run(spec, stdin_bytes);

  DrainAdapterStderr(result.stderr_text, adapter, notification_bridge);

  if (IsCancellationRequested(shutdown_flag, request_cancel_flag)) {
    if (error_out != nullptr) {
      error_out->clear();
    }
    return std::nullopt;
  }

  if (!ValidateAdapterProcessResult(result, adapter, error_out)) {
    return std::nullopt;
  }

  auto candidates = vinput::postprocess::ExtractCandidatesAuto(result.stdout_text, max_candidates);
  if (candidates.empty()) {
    VLOG_WARN("Adapter '{}' returned no valid candidates", adapter.id);
    return std::nullopt;
  }

  return candidates;
}

void AppendCandidate(vinput::result::Payload& payload, std::string text, const char* source) {
  text = std::string(vinput::str::Trim(text));
  if (text.empty()) {
    return;
  }

  payload.candidates.push_back(
      vinput::result::Candidate{.text = std::move(text), .source = source});
}

std::string CollectLlmCandidates(vinput::result::Payload& payload, std::set<std::string>& seen,
                                 std::span<const std::string> rewritten, int max_candidates) {
  std::string first_llm_text;
  int accepted = 0;
  for (const auto& text : rewritten) {
    std::string trimmed(vinput::str::Trim(text));
    if (trimmed.empty()) {
      continue;
    }
    if (seen.insert(trimmed).second) {
      if (first_llm_text.empty()) {
        first_llm_text = trimmed;
      }
      payload.candidates.push_back(vinput::result::Candidate{.text = std::move(trimmed),
                                                             .source = vinput::result::kSourceLlm});
      ++accepted;
      if (accepted >= max_candidates) {
        break;
      }
    }
  }
  return first_llm_text;
}

std::optional<std::string>
PrepareCommandTaskPrompt(std::string task_prompt, std::string_view normalized_asr,
                         std::string_view selected_text, std::string_view* asr_var_out,
                         std::string_view* selected_var_out, std::string* error_out) {
  if (vinput::prompt_template::IsFileUri(task_prompt)) {
    std::string load_err;
    auto loaded = vinput::prompt_template::LoadFromFileUri(task_prompt, &load_err);
    if (!loaded) {
      if (error_out != nullptr) {
        *error_out = fmt::sprintf(_("Failed to load prompt file '%s': %s"), task_prompt, load_err);
      }
      return std::nullopt;
    }
    task_prompt = std::move(*loaded);
  }

  if (vinput::prompt_template::HasInterpolation(task_prompt)) {
    *asr_var_out = normalized_asr;
    *selected_var_out = selected_text;
  } else {
    if (!task_prompt.empty() && task_prompt.back() != '\n') {
      task_prompt.push_back('\n');
    }
    task_prompt += '\n';
    task_prompt += WrapXmlBlock(kXmlTagAsr, normalized_asr);
    task_prompt += "\n\n";
    task_prompt += WrapXmlBlock(kXmlTagSelected, selected_text);
    task_prompt += '\n';
  }
  return task_prompt;
}

std::optional<std::vector<std::string>>
ExecuteCommandRewrite(const std::string& normalized_asr, const std::string& selected_text,
                      const vinput::scene::Definition& command_scene, const LlmProvider* provider,
                      const LlmAdapter* adapter, int max_candidates,
                      vinput::notify::NotificationBridge* notification_bridge,
                      const std::atomic<bool>* shutting_down, const std::atomic<bool>* cancel_flag,
                      std::string* error_out) {
  if (provider != nullptr) {
    std::string_view asr_var;
    std::string_view selected_var;
    auto task_prompt = PrepareCommandTaskPrompt(command_scene.prompt, normalized_asr, selected_text,
                                                &asr_var, &selected_var, error_out);
    if (!task_prompt) {
      return std::nullopt;
    }
    return RewriteWithOpenAiCompatible("", command_scene, *provider, max_candidates, error_out,
                                       *task_prompt, shutting_down, cancel_flag, asr_var,
                                       selected_var);
  }

  const glz::json_t cmd_input = {
      {"text", normalized_asr},
      {"selected", selected_text},
  };
  const std::string cmd_stdin = vinput::json::ToText(cmd_input);
  return RewriteWithCommandAdapter(cmd_stdin, command_scene, *adapter, max_candidates,
                                   notification_bridge, error_out, shutting_down, cancel_flag);
}

} // namespace

PostProcessor::PostProcessor(vinput::notify::NotificationBridge* notification_bridge)
    : notification_bridge_(notification_bridge) {}

void PostProcessor::SetNotificationBridge(vinput::notify::NotificationBridge* notification_bridge) {
  notification_bridge_ = notification_bridge;
}

void PostProcessor::Shutdown() {
  shutting_down_.store(true, std::memory_order_relaxed);
}

vinput::result::Payload PostProcessor::Process(const std::string& raw_text,
                                               const vinput::scene::Definition& scene,
                                               const CoreConfig& settings, std::string* error_out,
                                               const std::atomic<bool>* cancel_flag) const {
  const std::string normalized(vinput::str::Trim(raw_text));
  if (normalized.empty()) {
    return {};
  }

  const int max_llm_candidates = vinput::scene::NormalizeLlmMaxCandidates(scene.llm_max_candidates);

  vinput::result::Payload fallback;
  AppendCandidate(fallback, normalized, vinput::result::kSourceRaw);
  fallback.commitText = normalized;

  const CoreConfigView view(settings);
  const LlmProvider* provider = view.LlmProvider(scene.provider_id);
  const LlmAdapter* adapter = (provider == nullptr) ? view.LlmAdapter(scene.provider_id) : nullptr;
  if ((provider == nullptr && adapter == nullptr) || max_llm_candidates == 0) {
    return fallback;
  }

  std::optional<std::vector<std::string>> rewritten;
  if (provider != nullptr) {
    rewritten = RewriteWithOpenAiCompatible(normalized, scene, *provider, max_llm_candidates,
                                            error_out, {}, &shutting_down_, cancel_flag);
  } else {
    rewritten =
        RewriteWithCommandAdapter(normalized, scene, *adapter, max_llm_candidates,
                                  notification_bridge_, error_out, &shutting_down_, cancel_flag);
  }
  if (!rewritten.has_value()) {
    return fallback;
  }

  vinput::result::Payload payload;
  std::set<std::string> seen;

  if (scene.raw_cand) {
    AppendCandidate(payload, normalized, vinput::result::kSourceRaw);
    seen.insert(normalized);
  }

  const std::string first_llm_text =
      CollectLlmCandidates(payload, seen, *rewritten, max_llm_candidates);

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
  const std::string normalized_asr(vinput::str::Trim(asr_text));

  vinput::result::Payload fallback;
  const std::string& fallback_text = normalized_asr.empty() ? selected_text : normalized_asr;
  AppendCandidate(fallback, fallback_text, vinput::result::kSourceRaw);
  fallback.commitText = fallback_text;

  if (normalized_asr.empty() || selected_text.empty()) {
    return fallback;
  }

  const int command_max_llm_candidates =
      vinput::scene::NormalizeLlmMaxCandidates(command_scene.llm_max_candidates);

  const CoreConfigView view(settings);
  const LlmProvider* provider = view.LlmProvider(command_scene.provider_id);
  const LlmAdapter* adapter =
      (provider == nullptr) ? view.LlmAdapter(command_scene.provider_id) : nullptr;
  if ((provider == nullptr && adapter == nullptr) || command_max_llm_candidates == 0) {
    return fallback;
  }

  auto rewritten = ExecuteCommandRewrite(normalized_asr, selected_text, command_scene, provider,
                                         adapter, command_max_llm_candidates, notification_bridge_,
                                         &shutting_down_, cancel_flag, error_out);

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
  if (rewritten.has_value()) {
    first_llm_text = CollectLlmCandidates(payload, seen, *rewritten, command_max_llm_candidates);
  }

  payload.commitText = first_llm_text.empty() ? selected_text : first_llm_text;
  return payload;
}
