# Fork Contribution & Pre-PR Code Health Check SOP

This guide is for contributors and agents working in a **fork** of `fcitx5-vinput` to prepare pristine pull requests for upstream.

---

## 1. Upstream Remote Setup & Sync

In your local fork clone, configure the official repository as `upstream`:

```bash
# Add upstream if not already present
git remote add upstream https://github.com/xifan2333/fcitx5-vinput.git
git fetch upstream

# Always branch off the latest upstream/main
git checkout main
git fetch upstream
git rebase upstream/main
git push origin main

# Create clean feature branch
git checkout -b <type>/issue-<id>-<short-description> upstream/main
```

---

## 2. Pre-PR Code Health Check Pipeline

Before opening or finalizing a PR, run the full health check suite:

```
+-------------------------------------------------------------+
| 1. hk Static Quality & Formatting Check                     |
|    hk run check --safe -> hk fix (if formatting issues)     |
+------------------------------+------------------------------+
                               |
                +--------------v--------------+
                | 2. i18n & JSON Validation   |
                |    python3 scripts/check-i18n.py            |
                +--------------+--------------+
                               |
                +--------------v--------------+
                | 3. Git Diff Audit (Cleanliness Inspection)  |
                |    git diff upstream/main --stat            |
                +--------------+--------------+
                               |
                +--------------v--------------+
                | 4. Conventional Commit & PR |
                |    gh pr create --repo ...                  |
                +-----------------------------+
```

### Step 2.1: Run `hk` Code Quality & Formatter
```bash
# 1. Ensure hooks are active in fork
hk install --mise

# 2. Check changed files safely
{
  git diff --name-only -z upstream/main
  git diff --cached --name-only -z
  git ls-files --others --exclude-standard -z
} | hk run check --files0-from - --safe

# 3. Auto-fix formatting violations
hk fix

# 4. Full repository validation
hk check
```

### Step 2.2: Validate Translations & Configurations
```bash
# Verify Gettext .po files match src/ strings and .pot template
python3 scripts/check-i18n.py

# Verify default JSON configs parse correctly
python3 -m json.tool data/default-config.json >/dev/null
python3 -m json.tool notification.json >/dev/null
```

### Step 2.3: Staged Git Diff Audit (Cleanliness Checklist)
Run `git diff upstream/main --stat` and verify:
- [ ] No temporary files, build artifacts (`build/`), or editor files (`.vscode/`, `.idea/`).
- [ ] No hardcoded personal paths (e.g. `/home/username/...`).
- [ ] No accidentally committed secret tokens or API keys (`detect-private-key` in `hk` guards this).
- [ ] No merge conflict markers (`<<<<<<< HEAD`, `=======`).
- [ ] User-facing C++ strings are wrapped in `_("...")` for gettext localization.

---

## 3. Commit Message Hygiene (Conventional Commits)

Format: `<type>(<scope>): <concise imperative description> (#<issue_id>)`

- `feat`: New feature or enhancement (e.g. `feat(addon): add configurable indicator margin (#92)`)
- `fix`: Bug fix (e.g. `fix(daemon): handle PipeWire stream reconnection timeout (#88)`)
- `refactor`, `perf`, `docs`, `test`, `chore`

---

## 4. Submitting Clean PR to Upstream

```bash
git push -u origin <branch_name>

gh pr create \
  --repo xifan2333/fcitx5-vinput \
  --base main \
  --head <your_username>:<branch_name> \
  --title "<type>(<scope>): <summary> (#<issue_id>)" \
  --body "Closes #<issue_id>

### Summary of Changes
- Explanation of changes.

### Health-Check Verification
- [x] Passed \`hk check\` locally
- [x] Passed \`scripts/check-i18n.py\`
- [x] Audited git diff against upstream/main"
```

### Watch Upstream PR Checks
```bash
gh pr checks
```
If upstream `ci` or `cpp-linter` reports failure, fix locally with `hk fix`, commit, and push to fork branch.

---

## 5. Maintainer Review & Etiquette for External PRs

When reviewing and handling pull requests submitted by external contributors:

1. **Never Force-Push to Contributor Forks**: Even if GitHub's "Allow edits by maintainers" is technically enabled, maintainers and AI agents must **NEVER** run `git push -f` on an external contributor's personal repository branch. Overwriting their git history corrupts their local workspace and violates open-source trust boundaries.
2. **Handling Post-Merge Conflicts**: When merging an earlier PR introduces merge conflicts into another open PR from a contributor:
   - Politely leave a comment on the PR asking the author to rebase:
     > *"PR #<id> has merged into main, introducing a minor conflict. Could you please rebase your branch on latest main?"*
   - Keep the rebase and revision ownership with the original author.
3. **If Upstream Direct Integration is Required**: Resolve conflicts strictly on an upstream temporary branch without modifying or pushing to the contributor's fork remote.
