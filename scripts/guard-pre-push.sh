#!/usr/bin/env bash
# Guard against accidental pushes and force-pushes to external contributor forks.
# Integrated into hk via hk.pkl (hook.hk-pre-push).
set -euo pipefail

if [ "${VINPUT_ALLOW_FORK_PUSH:-0}" = "1" ]; then
  exit 0
fi

remote_name="${1:-}"
remote_url="${2:-}"

# If remote_url wasn't passed directly as argument, resolve from git config
if [ -z "${remote_url}" ] && [ -n "${remote_name}" ]; then
  remote_url="$(git config --get "remote.${remote_name}.url" || true)"
fi

# Normalize URL for checking
clean_url="$(echo "${remote_url}" | tr '[:upper:]' '[:lower:]' | sed -E 's|\.git$||')"

# Allowed official remotes for this repository
is_official_remote=false
if [[ "${clean_url}" =~ (github\.com[:/])?xifan2333/fcitx5-vinput$ ]] || [ -z "${remote_url}" ]; then
  is_official_remote=true
fi

# Read pushed refs from stdin (<local ref> <local oid> <remote ref> <remote oid>)
pushed_lines=()
if [ ! -t 0 ]; then
  while IFS= read -r line; do
    if [ -n "${line}" ]; then
      pushed_lines+=("${line}")
    fi
  done
fi

for line in "${pushed_lines[@]}"; do
  read -r local_ref local_oid remote_ref remote_oid <<< "${line}"

  # Check if this is a non-fast-forward (force push)
  is_force_push=false
  zero_oid="0000000000000000000000000000000000000000"
  if [ -n "${local_oid:-}" ] && [ -n "${remote_oid:-}" ] && [ "${remote_oid}" != "${zero_oid}" ] && [ "${local_oid}" != "${zero_oid}" ]; then
    if ! git merge-base --is-ancestor "${remote_oid}" "${local_oid}" 2>/dev/null; then
      is_force_push=true
    fi
  fi

  # Hard constraint: Never force-push to any external fork
  if [ "${is_official_remote}" = false ] && [ "${is_force_push}" = true ]; then
    cat >&2 <<EOF

================================================================================
[hk pre-push RED LINE VIOLATION]
Refusing to force-push (non-fast-forward) to external contributor fork:
  Remote: ${remote_name} (${remote_url})
  Target: ${remote_ref} (${remote_oid:0:7} -> ${local_oid:0:7})

Force-pushing to external contributor forks rewrites their commit history and
breaks their local workspace. This is strictly prohibited by AGENTS.md / CLAUDE.md.

If this PR has conflicts, please request the contributor to rebase via PR comment,
or resolve the merge on an upstream branch.
================================================================================

EOF
    exit 1
  fi

  # Policy constraint: Do not push to external contributor forks directly
  if [ "${is_official_remote}" = false ]; then
    cat >&2 <<EOF

================================================================================
[hk pre-push BLOCKED]
Refusing to push directly to external contributor repository:
  Remote: ${remote_name} (${remote_url})
  Target: ${remote_ref}

Maintainers and AI agents must not push commits to external contributor forks.
Please ask the contributor to update their branch, or integrate via upstream branches.
To bypass this guard if intentionally pushing, run with VINPUT_ALLOW_FORK_PUSH=1.
================================================================================

EOF
    exit 1
  fi
done

exit 0
