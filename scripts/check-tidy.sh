#!/usr/bin/env bash
# Local clang-tidy loop aligned with CI cpp-linter (clang-tidy 21).
#
#   scripts/check-tidy.sh           # lint .cpp files changed vs origin/main + working tree
#   scripts/check-tidy.sh --all     # lint every .cpp under src/ and tests/
#   scripts/check-tidy.sh --full    # lint with the Full (VINPUT_ENABLE_LOCAL_ASR) build;
#                                   # requires sherpa-onnx headers (Arch: yay -S sherpa-onnx-bin)
#   scripts/check-tidy.sh <file>…   # lint specific .cpp files
#
# Headers are analyzed through their including .cpp translation units.
# If only headers changed, pass --all or the including .cpp files explicitly.
#
# Local Qt may be newer than CI's Qt 6.4 (ubuntu-24.04). Prefer version-agnostic
# APIs (uint64_t over quint64, QString::fromLatin1 over QStringLiteral) and never
# include Qt headers introduced after Qt 6.4 (e.g. <QtCore/qtypes.h>).
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
    git diff --name-only origin/main...HEAD 2>/dev/null
    git diff --name-only
    git diff --cached --name-only
    git ls-files --others --exclude-standard
  } | grep -E '^(src|tests)/.*\.cpp$' | sort -u || true)
fi

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "no .cpp files to lint"
  exit 0
fi

if [ "$FULL" -eq 1 ]; then
  # sherpa-onnx may live in system paths (AUR) or a user prefix (mise run sherpa).
  sherpa_found=0
  for inc in /usr/include /usr/local/include "$HOME/.local/include" "${VINPUT_CMAKE_PREFIX_PATH:-}/include"; do
    if [ -f "$inc/sherpa-onnx/c-api/cxx-api.h" ]; then
      sherpa_found=1
      break
    fi
  done
  if [ "$sherpa_found" -eq 0 ]; then
    echo "error: full mode needs sherpa-onnx headers." >&2
    echo "hint: Arch: yay -S sherpa-onnx-bin;  other: mise run sherpa [prefix=~/.local]" >&2
    exit 1
  fi
  if ! grep -qF "sherpa_offline_backend.cpp" "$BUILD_DIR/compile_commands.json"; then
    echo "error: sherpa-onnx is installed, but the compile DB is Lite (no sherpa TUs)." >&2
    echo "hint: re-run 'mise run configure' (full mode) to regenerate the compile DB." >&2
    exit 1
  fi
else
  # Lite mode: skip the sherpa-onnx translation units (no headers locally).
  filtered=()
  for f in "${FILES[@]}"; do
    case "$f" in
    src/daemon/asr/backends/sherpa_*_backend.cpp | src/daemon/asr/hotword_utils.cpp | src/daemon/asr/vad_trimmer.cpp)
      echo "skip (needs sherpa-onnx, run with --full): $f" >&2
      ;;
    *) filtered+=("$f") ;;
    esac
  done
  FILES=("${filtered[@]}")
fi

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "no .cpp files to lint"
  exit 0
fi

printf '%s\0' "${FILES[@]}" | xargs -0 -r -n 1 -P "$(nproc)" clang-tidy -p "$BUILD_DIR"
