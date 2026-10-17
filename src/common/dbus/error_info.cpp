#include "common/dbus/error_info.h"

#include <cctype>
#include <regex>
#include <utility>

#include "common/utils/string_utils.h"

namespace vinput::dbus {

namespace {

using vinput::str::ConsumePrefix;

bool ParseQuotedValue(std::string_view text, std::string_view prefix, std::string_view* value,
                      std::string_view* tail) {
  if (!text.starts_with(prefix)) {
    return false;
  }
  text.remove_prefix(prefix.size());
  if (text.empty() || text.front() != '\'') {
    return false;
  }
  text.remove_prefix(1);
  const size_t pos = text.find('\'');
  if (pos == std::string_view::npos) {
    return false;
  }
  *value = text.substr(0, pos);
  if (tail) {
    *tail = text.substr(pos + 1);
  }
  return true;
}

ErrorInfo AdoptNested(ErrorInfo nested, std::string_view raw_message, std::string_view subject) {
  if (nested.raw_message.empty()) {
    nested.raw_message = std::string(raw_message);
  }
  if (nested.subject.empty() && !subject.empty()) {
    nested.subject = std::string(subject);
  }
  return nested;
}

ErrorInfo ClassifyKnownDetail(std::string_view text) {
  const std::string original(text);
  std::string_view value;
  std::string_view tail;

  if (ConsumePrefix(&text, "ASR provider error: ")) {
    return ClassifyErrorText(text);
  }
  if (ConsumePrefix(&text, "worker exception: ")) {
    return MakeErrorInfo(kErrorCodeUnknown, {}, {}, std::string(text));
  }

  if (ParseQuotedValue(text, "ASR provider ", &value, &tail) && ConsumePrefix(&tail, ": ")) {
    if (tail == "failed to start.") {
      return MakeErrorInfo(kErrorCodeAsrProviderStartFailed, std::string(value), {}, original);
    }
    if (ConsumePrefix(&tail, "failed to start. ")) {
      return MakeErrorInfo(kErrorCodeAsrProviderStartFailed, std::string(value),
                           vinput::str::TrimAsciiWhitespace(tail), original);
    }
    if (tail == "timed out.") {
      return MakeErrorInfo(kErrorCodeAsrProviderTimeout, std::string(value), {}, original);
    }
    if (ConsumePrefix(&tail, "timed out. ")) {
      return MakeErrorInfo(kErrorCodeAsrProviderTimeout, std::string(value),
                           vinput::str::TrimAsciiWhitespace(tail), original);
    }
    if (tail == "failed.") {
      return MakeErrorInfo(kErrorCodeAsrProviderFailed, std::string(value), {}, original);
    }
    if (ConsumePrefix(&tail, "failed. ")) {
      return MakeErrorInfo(kErrorCodeAsrProviderFailed, std::string(value),
                           vinput::str::TrimAsciiWhitespace(tail), original);
    }
    if (tail == "returned no text.") {
      return MakeErrorInfo(kErrorCodeAsrProviderNoText, std::string(value), {}, original);
    }
  }

  if (ParseQuotedValue(text, "Local ASR model check failed for provider ", &value, &tail)) {
    if (ConsumePrefix(&tail, ": ")) {
      return AdoptNested(ClassifyErrorText(tail), text, value);
    }
    return MakeErrorInfo(kErrorCodeLocalAsrModelCheckFailed, std::string(value), {}, original);
  }

  if (ParseQuotedValue(text, "Failed to initialize local ASR provider ", &value, &tail)) {
    if (ConsumePrefix(&tail, ": ")) {
      return AdoptNested(ClassifyErrorText(tail), text, value);
    }
    return MakeErrorInfo(kErrorCodeLocalAsrProviderInitFailed, std::string(value), {}, original);
  }

  if (ConsumePrefix(&text, "Failed to start recording: ")) {
    ErrorInfo nested = ClassifyErrorText(text);
    if (nested.code != kErrorCodeUnknown) {
      if (nested.raw_message.empty()) {
        nested.raw_message = original;
      }
      return nested;
    }
    return MakeErrorInfo(kErrorCodeStartRecordingFailed, {}, vinput::str::TrimAsciiWhitespace(text),
                         original);
  }
  if (text == "Failed to start recording.") {
    return MakeErrorInfo(kErrorCodeStartRecordingFailed, {}, {}, original);
  }

  if (ConsumePrefix(&text, "Failed to start command recording: ")) {
    ErrorInfo nested = ClassifyErrorText(text);
    if (nested.code != kErrorCodeUnknown) {
      if (nested.raw_message.empty()) {
        nested.raw_message = original;
      }
      return nested;
    }
    return MakeErrorInfo(kErrorCodeStartCommandRecordingFailed, {},
                         vinput::str::TrimAsciiWhitespace(text), original);
  }
  if (text == "Failed to start command recording.") {
    return MakeErrorInfo(kErrorCodeStartCommandRecordingFailed, {}, {}, original);
  }

  if (text == "Daemon is busy.") {
    return MakeErrorInfo(kErrorCodeDaemonBusy, {}, {}, original);
  }

  if (text == "ASR backend is still loading.") {
    return MakeErrorInfo(kErrorCodeAsrBackendLoading, {}, {}, original);
  }

  if (ConsumePrefix(&text, "Failed to apply ASR backend reload.")) {
    const std::string detail = vinput::str::TrimAsciiWhitespace(text);
    return MakeErrorInfo(kErrorCodeAsrBackendReloadFailed, {}, detail, original);
  }

  if (ConsumePrefix(&text, "Failed to reload ASR backend.")) {
    const std::string detail = vinput::str::TrimAsciiWhitespace(text);
    return MakeErrorInfo(kErrorCodeAsrBackendReloadFailed, {}, detail, original);
  }

  if (ConsumePrefix(&text, "missing 'vinput-model.json' in ")) {
    return MakeErrorInfo(kErrorCodeLocalAsrModelConfigMissing, {},
                         vinput::str::TrimAsciiWhitespace(text), original);
  }

  const std::string normalized_text = vinput::str::TrimAsciiWhitespace(text);
  std::string_view normalized(normalized_text);

  if (normalized == "Local ASR model configuration is missing.") {
    return MakeErrorInfo(kErrorCodeLocalAsrModelConfigMissing, {}, {}, original);
  }

  if (ParseQuotedValue(normalized, "Local ASR model configuration is missing for provider ", &value,
                       &tail) &&
      tail == ".") {
    return MakeErrorInfo(kErrorCodeLocalAsrModelConfigMissing, std::string(value), {}, original);
  }

  if (ParseQuotedValue(normalized, "", &value, &tail) &&
      ConsumePrefix(&tail, " is missing family for model '") && !tail.empty() &&
      tail.back() == '\'') {
    tail.remove_suffix(1);
    return MakeErrorInfo(kErrorCodeLocalAsrModelTypeMissing, std::string(value), std::string(tail),
                         original);
  }

  if (ParseQuotedValue(normalized, "", &value, &tail) &&
      ConsumePrefix(&tail, " contains invalid path for '")) {
    return MakeErrorInfo(kErrorCodeLocalAsrModelInvalidPath, std::string(value),
                         vinput::str::TrimAsciiWhitespace(tail), original);
  }

  if (ParseQuotedValue(normalized, "tokens file not found for model ", &value, nullptr)) {
    return MakeErrorInfo(kErrorCodeLocalAsrModelTokensMissing, std::string(value), {}, original);
  }

  if (ParseQuotedValue(normalized, "no model files found for model ", &value, nullptr)) {
    return MakeErrorInfo(kErrorCodeLocalAsrModelFilesMissing, std::string(value), {}, original);
  }

  if (ParseQuotedValue(normalized, "failed to resolve model root ", &value, &tail) &&
      ConsumePrefix(&tail, ": ")) {
    return MakeErrorInfo(kErrorCodeLocalAsrModelRootResolveFailed, std::string(value),
                         vinput::str::TrimAsciiWhitespace(tail), original);
  }

  if (ParseQuotedValue(normalized, "failed to parse ", &value, &tail) &&
      ConsumePrefix(&tail, ": ")) {
    return MakeErrorInfo(kErrorCodeLocalAsrModelParseFailed, std::string(value),
                         vinput::str::TrimAsciiWhitespace(tail), original);
  }

  if (ParseQuotedValue(normalized, "unsupported model family ", &value, nullptr)) {
    return MakeErrorInfo(kErrorCodeLocalAsrUnsupportedModelType, std::string(value), {}, original);
  }

  if (ParseQuotedValue(normalized, "failed to create sherpa-onnx recognizer for family ", &value,
                       nullptr)) {
    return MakeErrorInfo(kErrorCodeLocalAsrRecognizerCreateFailed, std::string(value), {},
                         original);
  }

  if (ParseQuotedValue(normalized, "failed to create VAD from ", &value, nullptr)) {
    return MakeErrorInfo(kErrorCodeVadCreateFailed, std::string(value), {}, original);
  }

  if (normalized == "audio capture loop is not initialized") {
    return MakeErrorInfo(kErrorCodeAudioCaptureLoopNotInitialized, {}, {}, original);
  }

  if (normalized == "failed to create PipeWire thread loop") {
    return MakeErrorInfo(kErrorCodePipeWireThreadLoopCreateFailed, {}, {}, original);
  }

  if (ConsumePrefix(&normalized, "failed to start PipeWire thread loop: ")) {
    return MakeErrorInfo(kErrorCodePipeWireThreadLoopStartFailed, {},
                         vinput::str::TrimAsciiWhitespace(normalized), original);
  }

  normalized = text;
  if (normalized == "failed to allocate PipeWire properties") {
    return MakeErrorInfo(kErrorCodePipeWirePropertiesAllocFailed, {}, {}, original);
  }

  if (normalized == "failed to create PipeWire stream") {
    return MakeErrorInfo(kErrorCodePipeWireStreamCreateFailed, {}, {}, original);
  }

  if (ConsumePrefix(&normalized, "failed to connect PipeWire stream: ")) {
    return MakeErrorInfo(kErrorCodePipeWireStreamConnectFailed, {},
                         vinput::str::TrimAsciiWhitespace(normalized), original);
  }

  normalized = text;
  if (ConsumePrefix(&normalized, "failed to create eventfd: ")) {
    return MakeErrorInfo(kErrorCodeDbusEventfdCreateFailed, {},
                         vinput::str::TrimAsciiWhitespace(normalized), original);
  }

  normalized = text;
  if (ConsumePrefix(&normalized, "failed to open user bus: ")) {
    return MakeErrorInfo(kErrorCodeDbusUserBusOpenFailed, {},
                         vinput::str::TrimAsciiWhitespace(normalized), original);
  }

  normalized = text;
  if (ConsumePrefix(&normalized, "failed to add D-Bus vtable: ")) {
    return MakeErrorInfo(kErrorCodeDbusVtableAddFailed, {},
                         vinput::str::TrimAsciiWhitespace(normalized), original);
  }

  normalized = text;
  if (ConsumePrefix(&normalized, "failed to request D-Bus name: ")) {
    return MakeErrorInfo(kErrorCodeDbusNameRequestFailed, {},
                         vinput::str::TrimAsciiWhitespace(normalized), original);
  }

  normalized = text;
  if (ConsumePrefix(&normalized, "LLM request failed: ")) {
    return MakeErrorInfo(kErrorCodeLlmRequestFailed, {},
                         vinput::str::TrimAsciiWhitespace(normalized), original);
  }

  normalized = text;
  if (ConsumePrefix(&normalized, "HTTP ")) {
    return MakeErrorInfo(kErrorCodeLlmHttpFailed, {}, vinput::str::TrimAsciiWhitespace(text),
                         original);
  }

  normalized = text;
  if (ConsumePrefix(&normalized, "Prompt file load failed: ")) {
    // Payload format: `<file_uri>: <reason>`. The reason may contain `: `
    // (e.g. errno strings), so a naive split is unsafe — anchor on the
    // `file:///` URI and consume up to the first whitespace.
    static const std::regex kPromptLoadRe(R"(^(file:///\S+):\s+(.+)$)");
    const std::string body = vinput::str::TrimAsciiWhitespace(normalized);
    std::smatch m;
    if (std::regex_match(body, m, kPromptLoadRe)) {
      return MakeErrorInfo(kErrorCodePromptFileLoadFailed, m[1].str(), m[2].str(), original);
    }
    return MakeErrorInfo(kErrorCodePromptFileLoadFailed, {}, body, original);
  }

  if (text == "Unknown error during processing") {
    return MakeErrorInfo(kErrorCodeProcessingUnknown, {}, {}, original);
  }

  return MakeRawError(original);
}

} // namespace

ErrorInfo MakeErrorInfo(std::string code, std::string subject, std::string detail,
                        std::string raw_message) {
  ErrorInfo info;
  info.code = std::move(code);
  info.subject = std::move(subject);
  info.detail = std::move(detail);
  info.raw_message = std::move(raw_message);
  return info;
}

ErrorInfo MakeRawError(std::string raw_message) {
  return MakeErrorInfo(kErrorCodeUnknown, {}, {}, std::move(raw_message));
}

ErrorInfo ClassifyErrorText(std::string_view message) {
  const std::string normalized = vinput::str::TrimAsciiWhitespace(message);
  if (normalized.empty()) {
    return {};
  }

  std::string_view text(normalized);
  if (ConsumePrefix(&text, "vinput-daemon: ") || ConsumePrefix(&text, "vinput: ")) {
    return ClassifyErrorText(text);
  }

  return ClassifyKnownDetail(text);
}

} // namespace vinput::dbus
