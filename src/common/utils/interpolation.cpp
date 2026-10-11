#include "common/utils/interpolation.h"

#include <cctype>
#include <cstdlib>

namespace vinput::interpolate {

namespace {

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

} // namespace

bool HasInterpolation(std::string_view text) {
  return text.find("${") != std::string_view::npos || text.find("{{") != std::string_view::npos;
}

std::string Interpolate(std::string_view text, const Resolver& resolver) {
  if (!resolver || text.empty()) {
    return std::string(text);
  }

  std::string result;
  result.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    // 1. Dollar syntax: `${...}`
    if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
      const size_t end = text.find('}', i + 2);
      if (end != std::string_view::npos) {
        const std::string_view raw = text.substr(i, end - i + 1);
        const std::string_view inner = Trim(text.substr(i + 2, end - (i + 2)));
        if (!inner.empty()) {
          Placeholder p{
              .raw = raw,
              .scheme = "env",
              .key = inner,
          };
          const auto replacement = resolver(p);
          if (replacement.has_value()) {
            result.append(*replacement);
            i = end + 1;
            continue;
          }
        }
      }
    }

    // 2. Double-brace syntax: `{{...}}`
    if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '{') {
      const size_t end = text.find("}}", i + 2);
      if (end != std::string_view::npos) {
        const std::string_view raw = text.substr(i, end - i + 2);
        const std::string_view inner = Trim(text.substr(i + 2, end - (i + 2)));
        if (!inner.empty()) {
          std::string_view scheme;
          std::string_view key = inner;

          // Check for `scheme.key` or `scheme:key`
          const size_t sep = inner.find_first_of(".:");
          if (sep != std::string_view::npos) {
            scheme = Trim(inner.substr(0, sep));
            key = Trim(inner.substr(sep + 1));
          }

          if (!key.empty()) {
            Placeholder p{
                .raw = raw,
                .scheme = scheme,
                .key = key,
            };
            const auto replacement = resolver(p);
            if (replacement.has_value()) {
              result.append(*replacement);
              i = end + 2;
              continue;
            }
          }
        }
      }
    }

    result.push_back(text[i]);
    ++i;
  }

  return result;
}

std::string InterpolateEnv(std::string_view text) {
  return Interpolate(text, [](const Placeholder& p) -> std::optional<std::string> {
    if (p.scheme == "env") {
      const std::string key(p.key);
      const char* val = std::getenv(key.c_str());
      return val != nullptr ? std::string(val) : std::string{};
    }
    return std::nullopt;
  });
}

} // namespace vinput::interpolate
