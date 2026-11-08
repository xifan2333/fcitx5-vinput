#!/usr/bin/env bash
# Analyzes cyclomatic and cognitive complexity across modified or specified C++ files.
# Uses lizard with cognitive complexity extension (-Ecognitive).
#
# Usage:
#   scripts/check-complexity.sh           # Audit changed .cpp/.h files vs origin/main + working tree
#   scripts/check-complexity.sh --all     # Audit all C++ files under src/
#   scripts/check-complexity.sh <file>…   # Audit specific files
set -euo pipefail

ALL=0
FILES=()
for arg in "$@"; do
  case "$arg" in
  --all) ALL=1 ;;
  *) FILES+=("$arg") ;;
  esac
done

if [ "$ALL" -eq 1 ]; then
  mapfile -t FILES < <(find src -name '*.cpp' -o -name '*.h' | sort)
elif [ "${#FILES[@]}" -eq 0 ]; then
  mapfile -t FILES < <({
    git diff --name-only origin/main...HEAD 2>/dev/null
    git diff --name-only
    git diff --cached --name-only
    git ls-files --others --exclude-standard
  } | grep -E '^(src|tests)/.*\.(cpp|h)$' | sort -u || true)
fi

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "No modified C++ files to audit for complexity."
  exit 0
fi

echo "Auditing complexity on ${#FILES[@]} file(s) (CCN <= 15, CogC <= 15)..."
lizard -Ecognitive -C 15 -T "cognitive_complexity=15" "${FILES[@]}"
