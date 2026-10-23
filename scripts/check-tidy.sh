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
  if ! git rev-parse --verify origin/main >/dev/null 2>&1; then
    echo "error: origin/main not found; pass explicit files or run --all instead." >&2
    exit 1
  fi
  mapfile -t changed_all < <({
    git diff --name-only origin/main...HEAD
    git diff --name-only
    git diff --cached --name-only
    git ls-files --others --exclude-standard
  } | grep -E '^(src|tests)/.*\.(cpp|h)$' | sort -u || true)
  mapfile -t FILES < <(printf '%s\n' "${changed_all[@]:-}" | grep '\.cpp$' || true)
  changed_headers=$(printf '%s\n' "${changed_all[@]:-}" | grep -c '\.h$' || true)
  if [ "${#FILES[@]}" -eq 0 ] && [ "$changed_headers" -gt 0 ]; then
    echo "warning: only headers changed; headers are linted through their including .cpp TUs." >&2
    echo "warning: no affected TU was identified automatically — run 'mise run check:tidy --all'" >&2
    echo "         (or pass specific .cpp files) to make sure the change is analyzed." >&2
  fi
fi

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "no .cpp files to lint"
  exit 0
fi

# Keep only TUs present in the local compilation database; report skips.
# Known CI-only TUs (need sherpa-onnx headers unavailable in the Lite DB):
CI_ONLY_TUS="daemon/asr/backends/sherpa_offline_backend.cpp daemon/asr/backends/sherpa_streaming_backend.cpp daemon/asr/hotword_utils.cpp daemon/asr/vad_trimmer.cpp"
lintable=()
missing_new=()
for f in "${FILES[@]}"; do
  if grep -qF "\"$(pwd)/$f\"" "$BUILD_DIR/compile_commands.json"; then
    lintable+=("$f")
  elif echo " $CI_ONLY_TUS " | grep -qF " ${f#src/} "; then
    echo "skip (sherpa-onnx TU, CI-only unless sherpa-onnx is installed): $f" >&2
  else
    echo "skip (not in compile DB — run 'mise run configure' to refresh): $f" >&2
    missing_new+=("$f")
  fi
done

if [ "${#lintable[@]}" -eq 0 ]; then
  echo "error: no lintable translation units found (compile DB may be stale; run 'mise run configure')." >&2
  exit 1
fi
if [ "${#missing_new[@]}" -gt 0 ]; then
  echo "warning: ${#missing_new[@]} changed file(s) skipped due to stale compile DB." >&2
fi

if [ "$FULL" -eq 1 ]; then
  cp "$BUILD_DIR/config.h" "$BUILD_DIR/.config.h.tidy-bak"
  # Always restore the Lite config.h, even on failure/interrupt.
  trap 'mv "$BUILD_DIR/.config.h.tidy-bak" "$BUILD_DIR/config.h"' EXIT
  sed -i 's/#define VINPUT_ENABLE_LOCAL_ASR 0/#define VINPUT_ENABLE_LOCAL_ASR 1/' "$BUILD_DIR/config.h"
  echo "full mode: VINPUT_ENABLE_LOCAL_ASR=1 (restored on exit)"
  echo "note: sherpa backend TUs (sherpa_offline/streaming_backend.cpp, hotword_utils.cpp,"
  echo "      vad_trimmer.cpp, ...) have no entries in the local Lite compile DB and"
  echo "      remain CI-only even in full mode."
fi

printf '%s\0' "${lintable[@]}" | xargs -0 -r -n 1 -P "$(nproc)" clang-tidy -p "$BUILD_DIR"
