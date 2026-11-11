#!/usr/bin/env bash
# Touched-file AST ratchet: files this branch modifies must be free of the
# mechanically decidable anti-patterns. Historical files stay untouched until a
# later branch reaches them.
#
# Advisory rules (nested-loops, anti-loop-alloc) keep their configured warning
# severity: legitimate fixed-size loops exist, so /vinput-review arbitrates them.
set -euo pipefail

ratchet_rules=(
  deep-if-branching
  prefer-switch-for-enum
  no-hardcoded-ui-strings
)

if [[ $# -eq 0 ]]; then
  exec ast-grep scan
fi

args=()
for rule in "${ratchet_rules[@]}"; do
  args+=("--error=${rule}")
done

exec ast-grep scan "${args[@]}" "$@"
