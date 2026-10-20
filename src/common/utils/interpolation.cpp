#include "common/utils/interpolation.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "common/utils/string_utils.h"

namespace vinput::interpolate {

namespace {

struct ParsedPlaceholder {
  Placeholder placeholder{};
  size_t length{0};
};

std::optional<ParsedPlaceholder> TryParsePlaceholder(std::string_view remaining,
                                                     bool& can_close_dollar,
                                                     bool& can_close_brace) noexcept {
  // 1. Dollar syntax: `${...}`
  if (can_close_dollar && remaining.starts_with("${")) {
    const size_t end = remaining.find('}', 2);
    if (end == std::string_view::npos) {
      can_close_dollar = false;
    } else {
      const std::string_view raw = remaining.substr(0, end + 1);
      const std::string_view inner = vinput::str::Trim(remaining.substr(2, end - 2));
      if (!inner.empty()) {
        return ParsedPlaceholder{
            .placeholder = {.raw = raw, .scheme = "env", .key = inner},
            .length = end + 1,
        };
      }
    }
  }

  // 2. Double-brace syntax: `{{...}}`
  if (can_close_brace && remaining.starts_with("{{")) {
    const size_t end = remaining.find("}}", 2);
    if (end == std::string_view::npos) {
      can_close_brace = false;
    } else {
      const std::string_view raw = remaining.substr(0, end + 2);
      const std::string_view inner = vinput::str::Trim(remaining.substr(2, end - 2));
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
          return ParsedPlaceholder{
              .placeholder = {.raw = raw, .scheme = scheme, .key = key},
              .length = end + 2,
          };
        }
      }
    }
  }

  return std::nullopt;
}

} // namespace

bool HasInterpolation(std::string_view text) noexcept {
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
    if (!can_close_dollar && !can_close_brace) {
      result.append(text.substr(i));
      break;
    }

    size_t next_marker = std::string_view::npos;
    if (can_close_dollar && can_close_brace) {
      next_marker = text.find_first_of("${", i);
    } else if (can_close_dollar) {
      next_marker = text.find('$', i);
    } else {
      next_marker = text.find('{', i);
    }

    if (next_marker == std::string_view::npos) {
      result.append(text.substr(i));
      break;
    }

    result.append(text.substr(i, next_marker - i));
    i = next_marker;

    if (auto parsed = TryParsePlaceholder(text.substr(i), can_close_dollar, can_close_brace)) {
      if (auto replacement = resolver(parsed->placeholder); replacement.has_value()) {
        result.append(*replacement);
        i += parsed->length;
        continue;
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
      constexpr size_t kStackBufSize = 128;
      if (p.key.size() < kStackBufSize) {
        char buf[kStackBufSize];
        std::memcpy(buf, p.key.data(), p.key.size());
        buf[p.key.size()] = '\0';
        const char* val = std::getenv(buf);
        return val != nullptr ? std::string(val) : std::string{};
      }
      const std::string key(p.key);
      const char* val = std::getenv(key.c_str());
      return val != nullptr ? std::string(val) : std::string{};
    }
    return std::nullopt;
  });
}

} // namespace vinput::interpolate
