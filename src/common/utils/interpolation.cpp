#include "common/utils/interpolation.h"

#include <cstddef>
#include <cstdlib>
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

std::optional<ParsedPlaceholder> TryParseDollarPlaceholder(std::string_view remaining,
                                                           bool& can_close_dollar) noexcept {
  if (!can_close_dollar || !remaining.starts_with("${")) {
    return std::nullopt;
  }

  const size_t end = remaining.find('}', 2);
  if (end == std::string_view::npos) {
    can_close_dollar = false;
    return std::nullopt;
  }

  const std::string_view inner = vinput::str::Trim(remaining.substr(2, end - 2));
  if (inner.empty()) {
    return std::nullopt;
  }

  return ParsedPlaceholder{
      .placeholder = {.raw = remaining.substr(0, end + 1), .scheme = "env", .key = inner},
      .length = end + 1,
  };
}

std::optional<ParsedPlaceholder> TryParseDoubleBracePlaceholder(std::string_view remaining,
                                                                bool& can_close_brace) noexcept {
  if (!can_close_brace || !remaining.starts_with("{{")) {
    return std::nullopt;
  }

  const size_t end = remaining.find("}}", 2);
  if (end == std::string_view::npos) {
    can_close_brace = false;
    return std::nullopt;
  }

  const std::string_view inner = vinput::str::Trim(remaining.substr(2, end - 2));
  if (inner.empty()) {
    return std::nullopt;
  }

  std::string_view scheme;
  std::string_view key = inner;
  const size_t sep = inner.find_first_of(".:");
  if (sep != std::string_view::npos) {
    scheme = vinput::str::Trim(inner.substr(0, sep));
    key = vinput::str::Trim(inner.substr(sep + 1));
    if (scheme.empty()) {
      return std::nullopt;
    }
  }

  if (key.empty()) {
    return std::nullopt;
  }

  return ParsedPlaceholder{
      .placeholder = {.raw = remaining.substr(0, end + 2), .scheme = scheme, .key = key},
      .length = end + 2,
  };
}

std::optional<ParsedPlaceholder> TryParsePlaceholder(std::string_view remaining,
                                                     bool& can_close_dollar,
                                                     bool& can_close_brace) noexcept {
  if (auto dollar = TryParseDollarPlaceholder(remaining, can_close_dollar); dollar.has_value()) {
    return dollar;
  }
  return TryParseDoubleBracePlaceholder(remaining, can_close_brace);
}

size_t FindNextCandidateMarker(std::string_view text, size_t pos, bool can_close_dollar,
                               bool can_close_brace) noexcept {
  if (can_close_dollar && can_close_brace) {
    return text.find_first_of("${", pos);
  }
  if (can_close_dollar) {
    return text.find('$', pos);
  }
  return text.find('{', pos);
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

    const size_t next_marker = FindNextCandidateMarker(text, i, can_close_dollar, can_close_brace);
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
      const std::string key(p.key);
      const char* val = std::getenv(key.c_str());
      return val != nullptr ? std::string(val) : std::string{};
    }
    return std::nullopt;
  });
}

} // namespace vinput::interpolate
