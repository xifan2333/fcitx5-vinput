#include "common/utils/version_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/utils/string_utils.h"

#include "config.h"

namespace vinput::version {

namespace {

bool CheckSingleConstraint(const SemanticVersion& current, std::string_view token) {
  token = vinput::str::Trim(token);
  if (token.empty() || token == "*") {
    return true;
  }

  enum class Op : std::uint8_t { Equal, Greater, GreaterEqual, Less, LessEqual };
  Op op = Op::Equal;

  if (token.starts_with(">=")) {
    op = Op::GreaterEqual;
    token.remove_prefix(2);
  } else if (token.starts_with("<=")) {
    op = Op::LessEqual;
    token.remove_prefix(2);
  } else if (token.starts_with('>')) {
    op = Op::Greater;
    token.remove_prefix(1);
  } else if (token.starts_with('<')) {
    op = Op::Less;
    token.remove_prefix(1);
  } else if (token.starts_with('=')) {
    op = Op::Equal;
    token.remove_prefix(1);
  }

  token = vinput::str::Trim(token);
  const auto target_opt = Parse(token);
  if (!target_opt.has_value()) {
    return false;
  }
  const auto& target = *target_opt;

  const auto cmp = current <=> target;
  switch (op) {
  case Op::Equal:
    return cmp == 0;
  case Op::Greater:
    return cmp > 0;
  case Op::GreaterEqual:
    return cmp >= 0;
  case Op::Less:
    return cmp < 0;
  case Op::LessEqual:
    return cmp <= 0;
  }
  return false;
}

} // namespace

std::optional<SemanticVersion> Parse(std::string_view str) {
  str = vinput::str::Trim(str);
  if (str.starts_with('v') || str.starts_with('V')) {
    str.remove_prefix(1);
  }

  const auto segments = vinput::str::Split(str, '.');
  if (segments.size() < 2 || segments.size() > 3) {
    return std::nullopt;
  }

  int major = 0;
  int minor = 0;
  int patch = 0;

  const auto res1 =
      std::from_chars(segments[0].data(), segments[0].data() + segments[0].size(), major);
  if (res1.ec != std::errc{} || res1.ptr != segments[0].data() + segments[0].size() || major < 0) {
    return std::nullopt;
  }

  const auto res2 =
      std::from_chars(segments[1].data(), segments[1].data() + segments[1].size(), minor);
  if (res2.ec != std::errc{} || res2.ptr != segments[1].data() + segments[1].size() || minor < 0) {
    return std::nullopt;
  }

  if (segments.size() == 3) {
    // If there is a pre-release dash, strip it for semantic comparison (e.g. 2.4.1-alpha)
    auto patch_str = segments[2];
    const size_t dash = patch_str.find('-');
    if (dash != std::string_view::npos) {
      patch_str = patch_str.substr(0, dash);
    }
    const auto res3 = std::from_chars(patch_str.data(), patch_str.data() + patch_str.size(), patch);
    if (res3.ec != std::errc{} || res3.ptr != patch_str.data() + patch_str.size() || patch < 0) {
      return std::nullopt;
    }
  }

  return SemanticVersion{.major = major, .minor = minor, .patch = patch};
}

bool Satisfies(std::string_view current_ver, std::string_view constraint) {
  constraint = vinput::str::Trim(constraint);
  if (constraint.empty() || constraint == "*") {
    return true;
  }

  const auto cur_opt = Parse(current_ver);
  if (!cur_opt.has_value()) {
    return false;
  }
  const auto& cur = *cur_opt;

  const auto tokens = vinput::str::Split(constraint, ',');
  return std::ranges::all_of(
      tokens, [&](std::string_view token) { return CheckSingleConstraint(cur, token); });
}

std::string_view CurrentHostVersion() {
  return VINPUT_VERSION;
}

} // namespace vinput::version
