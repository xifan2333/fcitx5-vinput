#include "daemon/postprocess/prompt_template.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>

#include "common/utils/debug_log.h"
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
  const std::string path(path_view);

  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    if (error != nullptr) {
      *error = std::string("stat failed: ") + std::strerror(errno);
    }
    return std::nullopt;
  }
  if (!S_ISREG(st.st_mode)) {
    return fail("not a regular file");
  }

  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    return fail("open failed");
  }

  // Read at most kMaxPromptFileBytes+1 bytes; the extra byte lets us detect
  // oversize without loading the whole file.
  std::string content;
  content.resize(kMaxPromptFileBytes + 1);
  ifs.read(content.data(), static_cast<std::streamsize>(content.size()));
  const std::streamsize got = ifs.gcount();
  const size_t read_bytes = got > 0 ? static_cast<size_t>(got) : 0;
  content.resize(read_bytes);

  if (content.size() > kMaxPromptFileBytes) {
    vinput::debug::Log("prompt_template: file '%s' truncated to %zu bytes (cap)\n", path.c_str(),
                       kMaxPromptFileBytes);
    content.resize(kMaxPromptFileBytes);
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
