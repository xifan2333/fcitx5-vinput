# Playbook: Fork Contribution & Synchronization

The canonical execution workflow for contributing to `fcitx5-vinput` from an external fork or reviewing community contributions.

> **Execution Contract**:
> Copy these steps verbatim into your task checklist (Todo List). Respect community contribution boundaries.

---

## Steps

1. **Remote Configuration**:
   - Verify upstream and origin remotes:
     ```bash
     git remote -v
     # Expected:
     # origin:   git@github.com:<contributor>/fcitx5-vinput.git
     # upstream: https://github.com/xifan2333/fcitx5-vinput.git
     ```
   - If `upstream` is missing:
     ```bash
     git remote add upstream https://github.com/xifan2333/fcitx5-vinput.git
     git fetch upstream
     ```

2. **Sync with Upstream Main**:
   - Rebase work branch against upstream main:
     ```bash
     git fetch upstream main
     git rebase upstream/main
     ```

3. **Strict Red Line: Zero Force-Push to Contributor Forks**:
   - *Never `git push -f` to external contributors' personal forks or branches*, even if "Allow edits by maintainers" is enabled.
   - Enforced by `hk.pkl` pre-push guard (`audit_push`). Overwriting contributor history breaks their local clone.
   - Resolve conflicts via clean local merge commits or request rebasing politely in the PR thread.

4. **Pre-PR Health Check**:
   - Run local validation before pushing:
     ```bash
     mise run check:changed
     mise run check:ast
     mise run check:dup
     mise run check:complexity
     mise run check:migration
     ```
   - Follow `playbooks/feature-or-fix.md` for opening and managing the PR.
