#pragma once

#include <optional>
#include <string_view>

namespace vinput::version {

struct SemanticVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;

  auto operator<=>(const SemanticVersion&) const = default;
};

std::optional<SemanticVersion> Parse(std::string_view str);

bool Satisfies(std::string_view current_ver, std::string_view constraint);

std::string_view CurrentHostVersion();

} // namespace vinput::version
