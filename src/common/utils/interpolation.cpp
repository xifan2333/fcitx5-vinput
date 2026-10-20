#include "common/utils/interpolation.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "common/utils/string_utils.h"

namespace vinput::interpolate {

bool HasInterpolation(std::string_view text) {
  return text.find("${") != std::string_view::npos || text.find("{{") != std::string_view::npos;
}

std::string Interpolate(std::string_view text, const Resolver& resolver) {
  if (!resolver || text.empty()) {
    return std::string(text);
  }

  std::string result;
  result.reserve(text.size());

  bool can_close_dollar = true;
  bool can_close_brace = true;

  size_t i = 0;
  while (i < text.size()) {
    const std::string_view remaining = text.substr(i);

    // 1. Dollar syntax: `${...}`
    if (can_close_dollar && remaining.starts_with("${")) {
      const size_t end = text.find('}', i + 2);
      if (end == std::string_view::npos) {
        can_close_dollar = false;
      } else {
        const std::string_view raw = text.substr(i, end - i + 1);
        const std::string_view inner = vinput::str::Trim(text.substr(i + 2, end - (i + 2)));
        if (!inner.empty()) {
          const Placeholder p{
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
    if (can_close_brace && remaining.starts_with("{{")) {
      const size_t end = text.find("}}", i + 2);
      if (end == std::string_view::npos) {
        can_close_brace = false;
      } else {
        const std::string_view raw = text.substr(i, end - i + 2);
        const std::string_view inner = vinput::str::Trim(text.substr(i + 2, end - (i + 2)));
        if (!inner.empty()) {
          std::string_view scheme;
          std::string_view key = inner;

          // Check for `scheme.key` or `scheme:key`
          const size_t sep = inner.find_first_of(".:");
          bool valid = true;
          if (sep != std::string_view::npos) {
            scheme = vinput::str::Trim(inner.substr(0, sep));
            key = vinput::str::Trim(inner.substr(sep + 1));
            if (scheme.empty()) {
              valid = false;
            }
          }

          if (valid && !key.empty()) {
            const Placeholder p{
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

    result.push_back(text.at(i));
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
