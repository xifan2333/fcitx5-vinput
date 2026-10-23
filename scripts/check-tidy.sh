#!/usr/bin/env bash
# Local clang-tidy loop aligned with CI cpp-linter (clang-tidy 21, full build).
#
# Usage:
#   scripts/check-tidy.sh                 # lint .cpp files changed vs origin/main + working tree
#   scripts/check-tidy.sh --all           # lint every .cpp under src/ and tests/
#   scripts/check-tidy.sh --full          # also analyze VINPUT_ENABLE_LOCAL_ASR code paths
#   scripts/check-tidy.sh <file> [...]    # lint specific files
#
# Notes:
# - Headers are analyzed through their including .cpp translation units.
# - sherpa backend .cpp files are absent from the local Lite compile DB and can
#   only be verified by CI (sherpa-onnx headers are not available locally).
# - Local Qt may be newer than CI's Qt 6.4 (ubuntu-24.04). Prefer
#   version-agnostic APIs (uint64_t over quint64, QString::fromLatin1 over
#   QStringLiteral) and never include Qt headers introduced after Qt 6.4.
set -euo pipefail

BUILD_DIR=build
FULL=0
ALL=0
FILES=()
for arg in "$@"; do
  case "$arg" in
  --full) FULL=1 ;;
  --all) ALL=1 ;;
  *) FILES+=("$arg") ;;
  esac
done

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
  echo "compile_commands.json not found in $BUILD_DIR. Run 'mise run configure' first." >&2
  exit 1
fi

ver=$(clang-tidy --version | sed -n 's/.*LLVM version \([0-9]*\).*/\1/p')
if [ "$ver" != "21" ]; then
  echo "warning: CI cpp-linter uses clang-tidy 21; local reports ${ver:-unknown}" >&2
fi

if [ "$ALL" -eq 1 ]; then
  mapfile -t FILES < <(find src tests -name '*.cpp' | sort)
elif [ "${#FILES[@]}" -eq 0 ]; then
  mapfile -t FILES < <({
    git diff --name-only origin/main...HEAD
    git diff --name-only
    git diff --cached --name-only
    git ls-files --others --exclude-standard
  } | grep -E '^(src|tests)/.*\.cpp$' | sort -u || true)
fi

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "no .cpp files to lint"
  exit 0
fi

# Keep only TUs present in the local compilation database; report skips.
lintable=()
for f in "${FILES[@]}"; do
  if grep -qF "\"$(pwd)/$f\"" "$BUILD_DIR/compile_commands.json"; then
    lintable+=("$f")
  else
    echo "skip (absent from local Lite compile DB, CI-only): $f" >&2
  fi
done

if [ "$FULL" -eq 1 ]; then
  cp "$BUILD_DIR/config.h" "$BUILD_DIR/.config.h.tidy-bak"
  # Always restore the Lite config.h, even on failure/interrupt.
  trap 'mv "$BUILD_DIR/.config.h.tidy-bak" "$BUILD_DIR/config.h"' EXIT
  sed -i 's/#define VINPUT_ENABLE_LOCAL_ASR 0/#define VINPUT_ENABLE_LOCAL_ASR 1/' "$BUILD_DIR/config.h"
  echo "full mode: VINPUT_ENABLE_LOCAL_ASR=1 (restored on exit)"
fi

printf '%s\0' "${lintable[@]}" | xargs -0 -r -n 1 -P "$(nproc)" clang-tidy -p "$BUILD_DIR"
