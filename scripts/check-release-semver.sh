#!/usr/bin/env bash
#
# Semantic Versioning Guard for fcitx5-vinput
# Enforces strict MAJOR.MINOR.PATCH increment rules based on git diff and commit history.
#

set -euo pipefail

target_version="${1:-}"
target_ref="${2:-HEAD}"

if [ -z "${target_version}" ]; then
  if [ -f "VERSION" ]; then
    target_version="$(tr -d '\n' < VERSION)"
  else
    echo "ERROR [semver-guard]: No target version provided and VERSION file not found." >&2
    exit 1
  fi
fi

# Strip leading 'v' if present
target_version="${target_version#v}"

# Validate semantic version syntax: strict non-negative integers without leading zeroes
semver_regex="^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
if [[ ! "${target_version}" =~ ${semver_regex} ]]; then
  echo "ERROR [semver-guard]: Target version '${target_version}' does not match strict Semantic Versioning syntax (MAJOR.MINOR.PATCH without leading zeroes, e.g. 2.4.0)." >&2
  exit 1
fi

t_major=$((10#${BASH_REMATCH[1]}))
t_minor=$((10#${BASH_REMATCH[2]}))
t_patch=$((10#${BASH_REMATCH[3]}))

# Resolve baseline tag excluding target_version itself (prevent self-baselining on tag push)
last_tag=""
if [ "${target_ref}" != "HEAD" ]; then
  last_tag="$(git describe --tags --abbrev=0 --match="v[0-9]*.[0-9]*.[0-9]*" --exclude="v${target_version}" --exclude="${target_version}" "${target_ref}^" 2>/dev/null || true)"
fi

if [ -z "${last_tag}" ]; then
  last_tag="$(git describe --tags --abbrev=0 --match="v[0-9]*.[0-9]*.[0-9]*" --exclude="v${target_version}" --exclude="${target_version}" "${target_ref}" 2>/dev/null || true)"
fi

if [ -z "${last_tag}" ]; then
  # Fallback to sorted release tags list
  last_tag="$(git tag -l "v[0-9]*.[0-9]*.[0-9]*" --sort=-v:refname | grep -v -E "^v?${target_version}$" | head -n 1 || true)"
fi

if [ -z "${last_tag}" ]; then
  echo "INFO [semver-guard]: No previous release tag found. Initial release allowed: v${target_version}"
  exit 0
fi

base_version="${last_tag#v}"
if [[ ! "${base_version}" =~ ${semver_regex} ]]; then
  echo "ERROR [semver-guard]: Baseline tag '${last_tag}' does not strictly match SemVer syntax." >&2
  exit 1
fi

b_major=$((10#${BASH_REMATCH[1]}))
b_minor=$((10#${BASH_REMATCH[2]}))
b_patch=$((10#${BASH_REMATCH[3]}))

# Gather changes between last_tag and target_ref
changed_files="$(git diff "${last_tag}..${target_ref}" --name-only || true)"
full_commits="$(git log "${last_tag}..${target_ref}" || true)"
commit_onelines="$(git log "${last_tag}..${target_ref}" --oneline || true)"

reasons=()
required_level="PATCH"

# 1. Check for MAJOR requirements (breaking protocol / breaking changes)
if echo "${full_commits}" | grep -Ei "BREAKING[ -]CHANGE:|^[a-f0-9]+ [a-z]+(\(.*\))?!:" >/dev/null 2>&1; then
  reasons+=("Explicit breaking change syntax detected in commit history (BREAKING CHANGE or feat!:).")
  required_level="MAJOR"
elif echo "${changed_files}" | grep -E "^(src/common/dbus/dbus_interface\.h|src/daemon/runtime/dbus_service\.cpp)$" >/dev/null 2>&1; then
  if git diff "${last_tag}..${target_ref}" -- "src/common/dbus/dbus_interface.h" "src/daemon/runtime/dbus_service.cpp" | \
     grep -E "^\-.*(kMethod|kSignal|SD_BUS_METHOD|SD_BUS_SIGNAL)" >/dev/null 2>&1; then
    reasons+=("Exported D-Bus method/signal definition removed or modified.")
    required_level="MAJOR"
  fi
fi

# 2. Check for MINOR requirements (if not already MAJOR)
if [ "${required_level}" != "MAJOR" ]; then
  # Rule Y-1: config_migration.cpp modified
  if echo "${changed_files}" | grep -E "^src/common/config/config_migration\.cpp$" >/dev/null 2>&1; then
    reasons+=("Configuration migration steps modified in src/common/config/config_migration.cpp.")
    required_level="MINOR"
  fi

  # Rule Y-2: configuration schema or serialization modified
  if echo "${changed_files}" | grep -E "^(src/common/config/core_config_types\.h|src/common/config/core_config_json\.cpp|data/default-config\.json)$" >/dev/null 2>&1; then
    reasons+=("Configuration schema modified in core_config_types.h, core_config_json.cpp, or default-config.json.")
    required_level="MINOR"
  fi

  # Rule Y-3: new subcommands or options added in src/cli/
  if echo "${changed_files}" | grep -E "^src/cli/" >/dev/null 2>&1; then
    if git diff "${last_tag}..${target_ref}" -- "src/cli" | grep -E "^\+.*add_subcommand|^\+.*add_option|^\+.*add_flag" >/dev/null 2>&1; then
      reasons+=("New CLI subcommands or option flags added in src/cli/.")
      required_level="MINOR"
    fi
  fi

  # Rule Y-4: conventional commit feat
  if echo "${commit_onelines}" | grep -E "^[a-f0-9]+ feat(\(.*\))?:" >/dev/null 2>&1; then
    reasons+=("New feature commit (feat:) detected in commit history.")
    required_level="MINOR"
  fi
fi

# 3. Validate target version against required_level
valid=true
recommended_version=""

case "${required_level}" in
  MAJOR)
    expected_major=$((b_major + 1))
    recommended_version="${expected_major}.0.0"
    if [ "${t_major}" -le "${b_major}" ] || [ "${t_minor}" -ne 0 ] || [ "${t_patch}" -ne 0 ]; then
      valid=false
    fi
    ;;

  MINOR)
    expected_minor=$((b_minor + 1))
    recommended_version="${b_major}.${expected_minor}.0"
    if [ "${t_major}" -eq "${b_major}" ]; then
      if [ "${t_minor}" -le "${b_minor}" ] || [ "${t_patch}" -ne 0 ]; then
        valid=false
      fi
    elif [ "${t_major}" -lt "${b_major}" ]; then
      valid=false
    fi
    ;;

  PATCH)
    expected_patch=$((b_patch + 1))
    recommended_version="${b_major}.${b_minor}.${expected_patch}"
    if [ "${t_major}" -ne "${b_major}" ] || [ "${t_minor}" -ne "${b_minor}" ] || [ "${t_patch}" -le "${b_patch}" ]; then
      valid=false
    fi
    ;;
esac

if [ "${valid}" = false ]; then
  cat >&2 <<EOF
================================================================================
❌ ERROR [hk semver-guard]: Release version 'v${target_version}' violates Semantic Versioning!
================================================================================
Base Tag:         v${base_version}
Target Version:   v${target_version}
Calculated Level: ${required_level}
Recommended Next: v${recommended_version}

Triggered Rules:
EOF
  for r in "${reasons[@]}"; do
    echo "  - ${r}" >&2
  done
  cat >&2 <<EOF

Enforcement Policy (Hard Constraint):
  - Configuration schema / migration changes or new features (feat:) REQUIRE a MINOR increment (e.g. v${b_major}.$((b_minor + 1)).0).
  - Only pure bugfixes (fix:), refactorings, or doc updates without schema changes qualify as a PATCH (e.g. v${b_major}.${b_minor}.$((b_patch + 1))).
  - Breaking protocol / D-Bus API removals REQUIRE a MAJOR increment (e.g. v$((b_major + 1)).0.0).

Please adjust the target version or VERSION file and retry.
================================================================================
EOF
  exit 1
fi

echo "✔ [semver-guard]: Target version 'v${target_version}' complies with Semantic Versioning (${required_level} level, baseline: v${base_version})."
exit 0
