#include "daemon/postprocess/prompt_template.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "common/utils/debug_log.h"
#include "common/utils/file_utils.h"
#include "common/utils/interpolation.h"

namespace vinput::prompt_template {

namespace {

constexpr std::string_view kFileUriPrefix = "file:///";
constexpr size_t kMaxPromptFileBytes = 256ULL * 1024ULL; // 256 KiB safety cap

} // namespace

bool IsFileUri(std::string_view s) {
  return s.starts_with(kFileUriPrefix);
}

bool HasInterpolation(std::string_view s) {
  return s.find("{{") != std::string_view::npos;
}

std::optional<std::string> LoadFromFileUri(std::string_view uri, std::string* error) {
  auto fail = [&](const char* msg) -> std::optional<std::string> {
    if (error != nullptr) {
      *error = msg;
    }
    return std::nullopt;
  };

  if (!IsFileUri(uri)) {
    return fail("not a file:/// URI");
  }
  // Strip `file://` (two slashes) but keep the leading `/` of the absolute
  // path. Result: `/abs/path`.
  const std::string_view path_view = uri.substr(kFileUriPrefix.size() - 1);
  if (path_view.empty() || path_view == "/") {
    return fail("empty path");
  }
  const std::filesystem::path path(path_view);

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (error != nullptr) {
      *error = ec ? ec.message() : "file does not exist";
    }
    return std::nullopt;
  }
  if (!std::filesystem::is_regular_file(path, ec)) {
    return fail("not a regular file");
  }

  std::string content;
  std::string read_error;
  if (!vinput::file::ReadTextFile(path, &content, &read_error, kMaxPromptFileBytes)) {
    if (error != nullptr) {
      *error = read_error;
    }
    return std::nullopt;
  }

  const auto fsize = std::filesystem::file_size(path, ec);
  if (!ec && fsize > kMaxPromptFileBytes) {
    VLOG_DEBUG("prompt_template: file '{}' truncated to {} bytes (cap)", path.string(),
               kMaxPromptFileBytes);
  }
  return content;
}

std::string Interpolate(std::string_view tpl, const Vars& vars) {
  return vinput::interpolate::Interpolate(
      tpl, [&vars](const vinput::interpolate::Placeholder& p) -> std::optional<std::string> {
        if (p.raw.starts_with("{{") && p.scheme.empty()) {
          if (p.key == "asr") {
            return std::string(vars.asr);
          }
          if (p.key == "selected") {
            return std::string(vars.selected);
          }
          if (p.key == "context") {
            return std::string(vars.context);
          }
        }
        return std::nullopt;
      });
}

} // namespace vinput::prompt_template
