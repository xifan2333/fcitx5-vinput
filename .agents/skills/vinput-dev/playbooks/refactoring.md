# Playbook: Refactoring Development

Canonical execution workflow for restructuring existing code while strictly preserving external behavior (100% behavioral equivalence) in `fcitx5-vinput`.

> **Execution Contract**:
> Structure changes. External behavior does NOT. If you uncover a bug or missing feature during refactoring, separate it into a distinct issue. Do not bundle behavior changes into a refactor. The Main Agent acts as the Single Writer.

---

## Steps

### 1. Pin the Contract First
- Explicitly verify what external interfaces and behaviors must stay invariant:
  - D-Bus method signatures and properties (`org.fcitx.Vinput`).
  - Glaze JSON schema fields in `config.json`.
  - Three-stream helper contracts (`stdin`, `stdout`, `stderr` notification line).
  - PipeWire real-time lock-free audio queue contracts.

*Completion criterion*: Invariant interfaces identified and pinned against tests or characterization assertions.

### 2. Subtract Before You Add (Task 1 Iron Rule)
- Before introducing any new abstractions, structs, or helpers:
  - Delete dead code and uncalled static helpers.
  - Collapse one-caller wrappers and redundant pass-through layers.
  - Drop unused parameters and unnecessary type conversions.
- The first atomic commit of a refactor should measurably shrink or simplify the existing code.

*Completion criterion*: Dead code and one-caller wrappers deleted in an isolated first commit.

### 3. Pre-Create Draft PR
- Push an empty commit and open a Draft PR:
  ```bash
  git commit --allow-empty -m "chore: initialize refactor for #<issue-id>"
  git push -u origin <branch-name>
  gh pr create --draft \
    --title "refactor(<scope>): <concise description> (#<issue-id>)" \
    --body "Part of #<issue-id>

### Implementation Tasks
- [ ] 1. Subtraction: delete obsolete helpers and collapse wrappers
- [ ] 2. Structure reshape: introduce deep module / table-driven dispatch
- [ ] 3. Migrate all callers and delete legacy APIs (zero shims)
- [ ] 4. Clean-up & verification of behavioral equivalence

*AI-assisted — Tool: <tool>; model: <model>; version: <ver>.*"
  ```

*Completion criterion*: Draft PR active with all checklist tasks unchecked (`- [ ]`).

### 4. Migrate Callers Then Delete Legacy APIs in One Wave (No Shims)
- Within this repository's codebase, **never introduce temporary backward-compatibility shims or parallel paths**.
- Migrate every caller in the same wave and immediately delete the legacy API.
- **Boy-Scout AST Ratchet**: `mise run check:changed` enforces zero AST violations on touched files (`ast-grep scan --error`). Any `deep-if-branching` or `nested-loops` in touched files must be resolved before committing. Untouched historical files are ignored.
- Run Deletion Test: verify that deleting the old wrapper concentrates complexity rather than scattering it.
- Run `mise run check:changed` on each atomic task commit.
- Update Draft PR checklist (`- [x]`) in lockstep.

*Completion criterion*: Legacy APIs deleted, all callers migrated to the new deep module, zero compatibility shims left, zero AST errors on touched files.

### 5. Verify Reader-Load Delta & Yield (S3: GATE_YIELD)
- Refactoring is only successful if it visibly reduces reader load (`principle-minimize-reader-load`). If the resulting code is harder to trace, revert.
- Run `mise run check:dup` to ensure no copy-paste duplication was introduced.
- Run `/vinput-review` for dual-axis verification.
- Execute `gh pr ready <pr-id>` and yield control. Zero polling.

*Completion criterion*: Diff lowers reader load, passes `check:dup` and `/vinput-review`, PR marked ready, session halted.
