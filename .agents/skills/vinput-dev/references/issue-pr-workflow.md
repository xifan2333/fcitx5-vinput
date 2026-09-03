# Issue + PR Driven Development Workflow (SOP)

Development must follow a strict, chronological **Pre-Code Draft PR -> Single-Item Loop -> Merge** process.

---

## 1. Dual-Planning Model

To avoid confusing functional planning with toolchain validation:

1. **Phase A: Task Planning (Pre-development - BEFORE CODING)**:
   - Defining *what* to build: Issue analysis, module boundaries, and opening the Draft PR with an unchecked `- [ ]` checklist.
2. **Phase B: Quality Gate Pre-check (Post-edit - AFTER EDIT)**:
   - Previewing *which* linter steps will execute on edited files via `mise run check:plan`.

---

## 2. Chronological Lifecycle

```
+------------------------------------------------------------------------+
| 1. Pre-Code Initialization (MANDATORY BEFORE ANY CODE IS WRITTEN)       |
|    gh issue view <id>                                                  |
|    git checkout -b <type>/issue-<id>-<name>                            |
|    git commit --allow-empty -m "chore: initialize draft pr for #<id>"   |
|    git push -u origin <type>/issue-<id>-<name>                         |
|    gh pr create --draft (ALL tasks unchecked: - [ ])                   |
+-----------------------------------+------------------------------------+
                                    |
                +-------------------v-------------------+
                | 2. Single-Item Focused Development    |
                |    Only implement the first - [ ]     |
                +-------------------+-------------------+
                                    |
                +-------------------v-------------------+
                | 3. Local Quality Gate & Pre-check     |
                |    mise run check:plan (preview steps)|
                |    mise run check:changed             |
                |    mise run fix (if needed)           |
                +-------------------+-------------------+
                                    |
                +-------------------v-------------------+
                | 4. Atomic Commit + Check Off + Push   |
                |    git add <files>                    |
                |    git commit -m "feat(scope): ..."   |
                |    git push                           |
                |    gh pr edit --body (update to - [x])|
                +-------------------+-------------------+
                                    | (Remaining tasks?)
                                    +-------- Yes -------+
                                    | No                 |
+-----------------------------------v-------------------+|
| 5. Full Validation, Ready & Merge                     ||
|    gh pr checks (verify PR CI passed)                 ||
|    gh pr ready (mark as ready for review)             ||
|    gh pr merge --squash --delete-branch               ||
+-------------------------------------------------------+|
                                    ^                    |
                                    +--------------------+
```

---

## 3. Detailed Execution Steps

### Phase 1: Pre-Code Initialization
```bash
# 1. Inspect issue
gh issue view <issue_id>

# 2. Create feature branch
git checkout -b feat/issue-<id>-<short-description>

# 3. Initialize branch with empty commit and push
git commit --allow-empty -m "chore: initialize draft pr for issue #<issue_id>"
git push -u origin feat/issue-<id>-<short-description>

# 4. Open Draft PR with ALL tasks UNCHECKED (- [ ])
gh pr create --draft \
  --title "<type>: <concise description> (#<issue_id>)" \
  --body "Closes #<issue_id>

### Implementation Tasks
- [ ] 1. Core data structures & config in src/common
- [ ] 2. Daemon audio stream handling in src/daemon
- [ ] 3. Update i18n & unit tests"
```

### Phase 2: Single-Item Execution Loop
For each unchecked `- [ ]` task in order:
1. **Code ONLY Task N**: Pick ONLY the topmost unchecked `- [ ]` item.
2. **Quality Gate**:
   ```bash
   mise run check:plan
   mise run check:changed
   # For iterative diagnostics:
   hk run check --safe --format jsonl
   ```
3. **Atomic Commit & Push**:
   ```bash
   git add <modified_files>
   git commit -m "<type>(<scope>): complete task N (#<issue_id>)"
   git push origin <branch>
   ```
4. **Update PR Checklist**:
   ```bash
   # Update Draft PR body to check off the completed task (- [x])
   gh pr edit --body "..."
   ```

### Phase 3: Finalize & Merge
```bash
# 1. Verify PR CI status
gh pr checks

# 2. (Optional for core changes) Trigger remote matrix dry build
gh workflow run release.yml && gh run watch

# 3. Mark PR ready and squash-merge
gh pr ready
gh pr merge --squash --delete-branch
```
