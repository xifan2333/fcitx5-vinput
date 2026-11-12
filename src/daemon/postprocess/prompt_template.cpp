#include "daemon/postprocess/prompt_template.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "common/utils/debug_log.h"
#include "common/utils/file_utils.h"
#include "common/utils/interpolation.h"

namespace vinput::prompt_template {

namespace {

constexpr std::string_view kFileUriPrefix = "file:///";
constexpr size_t kMaxPromptFileBytes = 256ULL * 1024ULL; // 256 KiB safety cap

void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    *error = message;
  }
}

std::optional<std::filesystem::path> ExtractFilePathFromFileUri(std::string_view uri,
                                                                std::string* error) {
  if (!IsFileUri(uri)) {
    SetError(error, "not a file:/// URI");
    return std::nullopt;
  }
  // Strip `file://` (two slashes) but keep the leading `/` of the absolute
  // path. Result: `/abs/path`.
  const std::string_view path_view = uri.substr(kFileUriPrefix.size() - 1);
  if (path_view.empty() || path_view == "/") {
    SetError(error, "empty path");
    return std::nullopt;
  }
  return std::filesystem::path(path_view);
}

bool ValidateRegularFilePath(const std::filesystem::path& path, std::string* error) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    SetError(error, ec ? ec.message() : "file does not exist");
    return false;
  }
  if (!std::filesystem::is_regular_file(path, ec)) {
    SetError(error, "not a regular file");
    return false;
  }
  return true;
}

void WarnIfTruncated(const std::filesystem::path& path) {
  std::error_code ec;
  const auto fsize = std::filesystem::file_size(path, ec);
  if (!ec && fsize > kMaxPromptFileBytes) {
    VLOG_DEBUG("prompt_template: file '{}' truncated to {} bytes (cap)", path.string(),
               kMaxPromptFileBytes);
  }
}

} // namespace

bool IsFileUri(std::string_view s) noexcept {
  return s.starts_with(kFileUriPrefix);
}

bool HasInterpolation(std::string_view s) noexcept {
  return s.find("{{") != std::string_view::npos;
}

std::optional<std::string> LoadFromFileUri(std::string_view uri, std::string* error) {
  const auto path = ExtractFilePathFromFileUri(uri, error);
  if (!path.has_value()) {
    return std::nullopt;
  }

  if (!ValidateRegularFilePath(*path, error)) {
    return std::nullopt;
  }

  auto read_res = vinput::file::ReadTextFile(*path, kMaxPromptFileBytes);
  if (!read_res) {
    SetError(error, read_res.error());
    return std::nullopt;
  }

  WarnIfTruncated(*path);
  return std::move(*read_res);
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
