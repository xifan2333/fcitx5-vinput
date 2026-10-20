#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace vinput::interpolate {

/// Representation of a parsed placeholder token in a string.
/// - `${VAR}`: raw="${VAR}", scheme="env", key="VAR"
/// - `{{env.VAR}}`: raw="{{env.VAR}}", scheme="env", key="VAR"
/// - `{{env:VAR}}`: raw="{{env:VAR}}", scheme="env", key="VAR"
/// - `{{asr}}`: raw="{{asr}}", scheme="", key="asr"
struct Placeholder {
  std::string_view raw;
  std::string_view scheme;
  std::string_view key;
};

/// Resolver callback for interpolation.
/// Returns replacement text when handled, or std::nullopt to preserve the
/// placeholder token verbatim.
using Resolver = std::function<std::optional<std::string>(const Placeholder& placeholder)>;

/// Checks whether `text` contains any interpolation placeholder (`${...}` or `{{...}}`).
[[nodiscard]] bool HasInterpolation(std::string_view text) noexcept;

/// Interpolates placeholders in `text` using `resolver`.
/// Unhandled placeholders (resolver returns std::nullopt) are preserved verbatim.
[[nodiscard]] std::string Interpolate(std::string_view text, const Resolver& resolver);

/// Resolves environment variables from `${VAR}` and `{{env.VAR}}` / `{{env:VAR}}`.
/// If the variable is unset in the process environment, it resolves to an empty string.
/// Plain text without env placeholders is returned unchanged.
[[nodiscard]] std::string InterpolateEnv(std::string_view text);

} // namespace vinput::interpolate
