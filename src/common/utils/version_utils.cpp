#include "common/utils/version_utils.h"

#include <algorithm>
#include <array>
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

  struct PrefixOp {
    std::string_view prefix;
    Op op;
  };

  static constexpr std::array<PrefixOp, 5> kPrefixOps = {{
      {">=", Op::GreaterEqual},
      {"<=", Op::LessEqual},
      {">", Op::Greater},
      {"<", Op::Less},
      {"=", Op::Equal},
  }};

  Op op = Op::Equal;
  for (const auto& entry : kPrefixOps) {
    if (token.starts_with(entry.prefix)) {
      op = entry.op;
      token.remove_prefix(entry.prefix.size());
      break;
    }
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

bool ParseNonNegativeInt(std::string_view sv, int& out) {
  const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return res.ec == std::errc{} && res.ptr == sv.data() + sv.size() && out >= 0;
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

  if (!ParseNonNegativeInt(segments[0], major) || !ParseNonNegativeInt(segments[1], minor)) {
    return std::nullopt;
  }

  if (segments.size() == 3) {
    // If there is a pre-release dash, strip it for semantic comparison (e.g. 2.4.1-alpha)
    auto patch_str = segments[2];
    const size_t dash = patch_str.find('-');
    if (dash != std::string_view::npos) {
      patch_str = patch_str.substr(0, dash);
    }
    if (!ParseNonNegativeInt(patch_str, patch)) {
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
