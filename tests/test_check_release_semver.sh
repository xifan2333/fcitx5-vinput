#!/usr/bin/env bash
#
# Unit tests for scripts/check-release-semver.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SEMVER_SCRIPT="${REPO_ROOT}/scripts/check-release-semver.sh"

TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TEMP_DIR}"' EXIT

cd "${TEMP_DIR}"
git init --quiet -b main
git config user.name "Tester"
git config user.email "tester@example.com"

# Helpers
assert_success() {
  local desc="$1"
  shift
  if "$@" >/dev/null 2>&1; then
    echo "  ✔ PASS: ${desc}"
  else
    echo "  ❌ FAIL (expected success): ${desc}"
    "$@" || true
    exit 1
  fi
}

assert_failure() {
  local desc="$1"
  shift
  if "$@" >/dev/null 2>&1; then
    echo "  ❌ FAIL (expected failure): ${desc}"
    exit 1
  else
    echo "  ✔ PASS: ${desc}"
  fi
}

echo "=== Running Semantic Versioning Guard Tests ==="

# 1. Test syntax validation
echo "--- 1. Syntax Validation ---"
assert_success "valid standard 2.4.0" bash "${SEMVER_SCRIPT}" "2.4.0"
assert_success "valid prefixed v2.4.0" bash "${SEMVER_SCRIPT}" "v2.4.0"
assert_failure "reject leading zeroes 01.2.3" bash "${SEMVER_SCRIPT}" "01.2.3"
assert_failure "reject leading zeroes 1.02.3" bash "${SEMVER_SCRIPT}" "1.02.3"
assert_failure "reject leading zeroes 1.2.03" bash "${SEMVER_SCRIPT}" "1.2.03"
assert_failure "reject non-numeric v2.4.0-alpha" bash "${SEMVER_SCRIPT}" "2.4.0-alpha"

# Setup initial commit and base tag v2.3.27
mkdir -p src/common/config src/common/dbus src/daemon/runtime src/addon/dbus src/cli
echo "initial" > README.md
git add README.md
git commit -m "chore: initial commit" --quiet
git tag "v2.3.27"

# 2. Test Patch Release
echo "--- 2. Patch Release Logic ---"
echo "bug fix" >> README.md
git commit -am "fix: correct minor typo" --quiet

assert_success "accept next patch 2.3.28" bash "${SEMVER_SCRIPT}" "2.3.28"
assert_failure "reject same version 2.3.27" bash "${SEMVER_SCRIPT}" "2.3.27"
assert_failure "reject lower version 2.3.26" bash "${SEMVER_SCRIPT}" "2.3.26"
assert_failure "reject minor increment 2.4.0 for pure fix" bash "${SEMVER_SCRIPT}" "2.4.0"

# 3. Test Feature / MINOR Release
echo "--- 3. Minor Release Logic (feat:) ---"
echo "new feature" >> README.md
git commit -am "feat: implement shiny feature" --quiet

assert_success "accept next minor 2.4.0" bash "${SEMVER_SCRIPT}" "2.4.0"
assert_failure "reject next major 3.0.0 on pure minor change" bash "${SEMVER_SCRIPT}" "3.0.0"
assert_failure "reject patch version 2.3.28 on feat" bash "${SEMVER_SCRIPT}" "2.3.28"
assert_failure "reject skipped minor 2.5.0" bash "${SEMVER_SCRIPT}" "2.5.0"
assert_failure "reject mixed major transition 3.4.5" bash "${SEMVER_SCRIPT}" "3.4.5"

# 4. Test Config Schema Change (src/common/config/vinput_config.h)
echo "--- 4. Config Schema Change Detection ---"
git checkout -b branch-config "v2.3.27" --quiet
echo "// config change" >> src/common/config/vinput_config.h
git add src/common/config/vinput_config.h
git commit -m "refactor(config): update addon config field" --quiet

assert_success "config change triggers MINOR 2.4.0" bash "${SEMVER_SCRIPT}" "2.4.0"
assert_failure "config change rejects PATCH 2.3.28" bash "${SEMVER_SCRIPT}" "2.3.28"

# 5. Test Breaking Bang Commit (fix!:)
echo "--- 5. Breaking Commit Bang Pattern (fix!:) ---"
git checkout -b branch-bang "v2.3.27" --quiet
echo "breaking change" >> README.md
git commit -am "fix!: break backward compatibility in core" --quiet

assert_success "fix!: triggers MAJOR 3.0.0" bash "${SEMVER_SCRIPT}" "3.0.0"
assert_failure "fix!: rejects MINOR 2.4.0" bash "${SEMVER_SCRIPT}" "2.4.0"
assert_failure "fix!: rejects PATCH 2.3.28" bash "${SEMVER_SCRIPT}" "2.3.28"

# 6. Test Breaking Footer (BREAKING CHANGE:)
echo "--- 6. Breaking Footer Detection ---"
git checkout -b branch-footer "v2.3.27" --quiet
echo "breaking footer" >> README.md
git commit -am $'chore: adjust internal pipeline\n\nBREAKING CHANGE: overhaul pipeline protocol' --quiet

assert_success "BREAKING CHANGE: triggers MAJOR 3.0.0" bash "${SEMVER_SCRIPT}" "3.0.0"
assert_failure "BREAKING CHANGE: rejects MINOR 2.4.0" bash "${SEMVER_SCRIPT}" "2.4.0"

# 7. Test CLI Subcommand / Option Removal (MAJOR) and Help Edit (PATCH)
echo "--- 7. CLI Option Removal & Modification ---"
git checkout -b branch-cli "v2.3.27" --quiet
cat << 'EOF' > src/cli/test_cli.cpp
app.add_option("--old-flag", "old option");
auto* sub = app.add_subcommand("test", "test command");
sub->alias("t");
EOF
git add src/cli/test_cli.cpp
git commit -m "feat: add cli options" --quiet
git tag "v2.4.0"

# 7a. Modify description only (should remain PATCH)
sed -i 's/old option/improved description/' src/cli/test_cli.cpp
git commit -am "docs: update option description" --quiet
assert_success "CLI help text edit only remains PATCH 2.4.1" bash "${SEMVER_SCRIPT}" "2.4.1"
assert_failure "CLI help text edit rejects unexpected MAJOR 3.0.0" bash "${SEMVER_SCRIPT}" "3.0.0"

# 7b. Alias removal triggers MAJOR
sed -i '/alias("t")/d' src/cli/test_cli.cpp
git commit -am "chore: remove alias t" --quiet
assert_success "CLI alias removal triggers MAJOR 3.0.0" bash "${SEMVER_SCRIPT}" "3.0.0"
assert_failure "CLI alias removal rejects PATCH 2.4.2" bash "${SEMVER_SCRIPT}" "2.4.2"

# 7c. Option removal triggers MAJOR
sed -i '/--old-flag/d' src/cli/test_cli.cpp
git commit -am "chore: remove old flag" --quiet

assert_success "CLI removal triggers MAJOR 3.0.0" bash "${SEMVER_SCRIPT}" "3.0.0"
assert_failure "CLI removal rejects MINOR 2.5.0" bash "${SEMVER_SCRIPT}" "2.5.0"
assert_failure "CLI removal rejects PATCH 2.4.3" bash "${SEMVER_SCRIPT}" "2.4.3"

# 8. Test D-Bus Interface Modifications
echo "--- 8. D-Bus Interface Removal and Additions ---"
git checkout -b branch-dbus "v2.4.0" --quiet
cat << 'EOF' > src/addon/dbus/notifier_dbus_object.h
FCITX_OBJECT_VTABLE_METHOD(Notify, vinput::dbus::kMethodNotify, vinput::dbus::kErrorInfoSignature, "");
EOF
git add src/addon/dbus/notifier_dbus_object.h
git commit -m "feat: add notifier dbus object" --quiet
git tag "v2.5.0"

# 8a. Adding a new D-Bus method under a non-feat commit triggers MINOR
cat << 'EOF' >> src/addon/dbus/notifier_dbus_object.h
FCITX_OBJECT_VTABLE_METHOD(NotifyExtra, vinput::dbus::kMethodNotifyExtra, "", "");
EOF
git commit -am "chore: expose extra notifier dbus method" --quiet
assert_success "New D-Bus method triggers MINOR 2.6.0" bash "${SEMVER_SCRIPT}" "2.6.0"
assert_failure "New D-Bus method rejects PATCH 2.5.1" bash "${SEMVER_SCRIPT}" "2.5.1"
git tag "v2.6.0"

# 8b. Removing D-Bus method triggers MAJOR
sed -i '/FCITX_OBJECT_VTABLE_METHOD(Notify,/d' src/addon/dbus/notifier_dbus_object.h
git commit -am "refactor: drop notifier dbus method" --quiet

assert_success "D-Bus removal triggers MAJOR 3.0.0" bash "${SEMVER_SCRIPT}" "3.0.0"
assert_failure "D-Bus removal rejects MINOR 2.7.0" bash "${SEMVER_SCRIPT}" "2.7.0"
assert_failure "D-Bus removal rejects PATCH 2.6.1" bash "${SEMVER_SCRIPT}" "2.6.1"

# 8c. Multiline signature modification triggers MAJOR
git checkout -b branch-dbus-multiline "v2.5.0" --quiet
cat << 'EOF' > src/addon/dbus/notifier_dbus_object.h
FCITX_OBJECT_VTABLE_METHOD(
    Notify,
    vinput::dbus::kMethodNotify,
    vinput::dbus::kErrorInfoSignature,
    ""
);
EOF
git add src/addon/dbus/notifier_dbus_object.h
git commit -m "feat: use multiline notifier macro" --quiet
git tag "v2.7.0"

sed -i 's/""/"s"/' src/addon/dbus/notifier_dbus_object.h
git commit -am "refactor: alter output signature parameter on continuation line" --quiet

assert_success "Multiline D-Bus signature modification triggers MAJOR 3.0.0" bash "${SEMVER_SCRIPT}" "3.0.0"
assert_failure "Multiline D-Bus signature modification rejects PATCH 2.7.1" bash "${SEMVER_SCRIPT}" "2.7.1"
assert_failure "Multiline D-Bus signature modification rejects MINOR 2.8.0" bash "${SEMVER_SCRIPT}" "2.8.0"

# 9. Test Shallow Clone Detection
echo "--- 9. Shallow Clone Guard ---"
SHALLOW_DIR="$(mktemp -d)"
git clone --depth 1 "file://${TEMP_DIR}" "${SHALLOW_DIR}" --quiet
assert_failure "reject release validation on shallow clone" bash -c "cd '${SHALLOW_DIR}' && bash '${SEMVER_SCRIPT}' 3.0.0"
rm -rf "${SHALLOW_DIR}"

echo "=== All Semantic Versioning Guard Tests Passed Successfully! ==="
