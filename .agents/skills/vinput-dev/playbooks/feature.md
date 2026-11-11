# Playbook: Feature Development

Canonical execution workflow for introducing new capabilities or altering external behavior in `fcitx5-vinput`.

> **Execution Contract**:
> Copy these steps into your task checklist. Advance one item at a time. The Main Agent acts as the Single Writer for the feature branch.

---

## Steps

### 1. Upfront Grounding (S0: EXPLORE)
- Run `Grounding Scout` (or query Context7 / local source) to inspect existing subsystem architecture and call chains.
- Ground domain concepts against `GLOSSARY.md`.
- Settle behavioral or protocol forks via `/tmp/vinput-proto-*` prototypes (Empirical Fork Rule: never ask humans what can be measured).

*Completion criterion*: Subsystem call chain mapped, domain types aligned with `GLOSSARY.md`, and behavioral forks empirically verified.

### 2. Domain Modeling & Interface Design (Design-it-Twice)
- **Model the Domain**: Define domain data structures before writing business logic. Replace scattered boolean flags with state machines, table-driven lookups, or strongly-typed enums.
- **Design-it-Twice**: When crossing module boundaries (e.g. `daemon` to `common`), draft at least two distinct interface signatures. Select the deeper interface that hides internal complexity behind a compact surface.
- Audit configuration impact: if modifying `config.json` or `vinput.conf` schema, add a versioned migration step in `src/common/config/config_migration.cpp`.

*Completion criterion*: Domain types defined, deepest interface selected, and `ConfigMigration` drafted if schema changed.

### 3. Pre-Create Draft PR & Task Slicing
- Push an empty commit to initialize the feature branch:
  ```bash
  git commit --allow-empty -m "chore: initialize <slug> (#<issue-id>)"
  git push -u origin <branch-name>
  ```
- Open a Draft PR with ALL checklist items UNCHECKED (`- [ ]`):
  ```bash
  gh pr create --draft \
    --title "feat(<scope>): <concise description> (#<issue-id>)" \
    --body "Closes #<issue-id>

### Implementation Tasks
- [ ] 1. Core data structures and config types in src/common
- [ ] 2. Implementation and stream wiring in target module
- [ ] 3. Unit tests and verification

*AI-assisted — Tool: <tool>; model: <model>; version: <ver>.*"
  ```

*Completion criterion*: Draft PR active with all checklist tasks unchecked (`- [ ]`).

### 4. Single-Item Atomic Implementation Loop (S2: TRACK_LOOP)
For each unchecked `- [ ]` task in order:
- Implement only the topmost item using the shortest working diff conforming to `principles/laziness-protocol.md`.
- **Scrap Detector**: If implementation requires $\ge 2$ special-case branches or breaks initial interface assumptions, abort immediately. Run `git reset`, report friction, and redesign.
- Run sub-second local quality gate:
  ```bash
  mise run check:changed
  ```
- Commit atomically referencing the task:
  ```bash
  git add <modified_files>
  git commit -m "feat(<scope>): <description of task N> (#<issue-id>)"
  git push origin <branch-name>
  ```
- Update Draft PR body in lockstep checking off `- [x]` for Task N:
  ```bash
  gh pr edit <pr-id> --body "..."
  ```

*Completion criterion*: Every task in the Draft PR checklist is implemented, verified locally, and marked `- [x]`.

### 5. Self-Review & Gate Yield (S3: GATE_YIELD)
- Run code duplication check:
  ```bash
  mise run check:dup
  ```
  If clone blocks are detected, report Type A vs Type B breakdown to maintainer and await decision (S2.5).
- Dispatch read-only Dual-Axis Reviewers (Spec + Standards) via `/vinput-review`.
- Mark PR ready:
  ```bash
  gh pr ready <pr-id>
  ```
- **Non-blocking Handover**: Output PR URL and CI run URL, then yield control immediately. Zero polling.

*Completion criterion*: PR marked ready, URLs output in chat, and session halted.
