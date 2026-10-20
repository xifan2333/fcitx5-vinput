#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vinput::prompt_template {

// Variables exposed to {{...}} interpolation in scene prompts.
struct Vars {
  std::string_view asr;      // ASR recognition result (raw text or voice command)
  std::string_view selected; // Command-mode selected source text; empty otherwise
  std::string_view context;  // pre-built recent input history block
};

// True when `s` begins with the standard `file:///` URI prefix.
[[nodiscard]] bool IsFileUri(std::string_view s) noexcept;

// True when `s` contains any `{{` marker — used to decide whether to run
// interpolation. We deliberately do not parse the variable name here.
[[nodiscard]] bool HasInterpolation(std::string_view s) noexcept;

// Load the absolute path embedded in a `file:///abs/path` URI. On failure
// returns nullopt and writes a short diagnostic into `error` when non-null.
// The on-disk read is capped by an internal size limit; oversize files are
// truncated and a warning is logged so a runaway file cannot OOM the daemon.
[[nodiscard]] std::optional<std::string> LoadFromFileUri(std::string_view uri, std::string* error);

// Substitute the supported variables (`{{asr}}`, `{{selected}}`, `{{context}}`)
// in `tpl`. Unknown placeholders are left intact, so the author can include
// literal `{{foo}}` without escaping.
[[nodiscard]] std::string Interpolate(std::string_view tpl, const Vars& vars);

} // namespace vinput::prompt_template
