# Playbook: Feature or Bug Fix Development

The canonical execution workflow for implementing any new feature, bugfix, or refactoring in `fcitx5-vinput`.

> **Execution Contract**:
> When entering this playbook, copy these steps verbatim into your task checklist (Todo List) before executing. Complete each step in order; mark completed items progressively.

---

## Steps

1. **Scope & Laziness Gate (Pre-action)**:
   - Apply `principles/laziness-protocol.md`: Define the smallest complete change that meets the objective. Reject unrequested abstractions or "for later" configuration toggles.
   - Consult `GLOSSARY.md` for domain terminology.
   - If touching C ABI boundaries (PipeWire, sd-bus, libarchive), check `principles/c-abi-boundaries.md`.
   - If introducing complex design changes, execute Phase 0 prior-art research and report design to the maintainer before coding.

2. **Branch & Worktree Isolation**:
   - Create a dedicated feature branch from `origin/main`:
     ```bash
     git fetch origin main
     # If current working tree is occupied, create a dedicated worktree:
     git worktree add ../fcitx5-vinput-<slug> -b <type>/<issue-id>-<slug> origin/main
     ```
   - *Red Line*: Never commit implementation code directly to `main`.

3. **Pre-Create Draft PR**:
   - Push branch and open a Draft PR with ALL checklist items UNCHECKED (`- [ ]`):
     ```bash
     git commit --allow-empty -m "chore: initialize <slug> (#<issue-id>)"
     git push -u origin <branch-name>
     gh pr create --draft --title "<type>(<scope>): <subject> (#<issue-id>)" --body "Part of #<issue-id>\n\n### Implementation Tasks\n- [ ] 1. <Task 1>\n- [ ] 2. <Task 2>\n\n*AI-assisted — Tool: <tool>; model: <model>; version: <ver>.*"
     ```

4. **Atomic Single-Item Implementation Loop**:
   For each unchecked task in the PR checklist:
   - Implement the change using the shortest working diff.
   - Run sub-second local quality gate:
     ```bash
     mise run check:changed
     ```
   - Commit atomically with a Conventional Commits message referencing the task and issue.
   - Push to branch and immediately update PR description checking off that task (`- [x]`) for real-time traceability.
   - Report progress in chat (e.g. "Completed 1 of 3 tasks; implementing task 2").

5. **Pre-PR Self-Review Gate**:
   - Run full repository checks:
     ```bash
     mise run check:ast        # Structural AST check
     mise run check:dup        # Duplication check (report to maintainer if clones found)
     mise run check:complexity # Lizard CCN/CogC <= 15
     mise run check:migration  # Config migration check
     ```
   - Audit diff against `CODING_STANDARDS.md` (no allocations in audio callbacks, no raw `system()`, no `// NOLINT`).

6. **Mark Ready & Non-Blocking Handover**:
   - Mark PR ready: `gh pr ready <pr-id>`.
   - *Red Line*: Prohibited from busy-waiting or polling CI (`gh run watch` or sleep loops). Output PR URL and CI run URL, summarize delivered changes, and yield control.
   - `pi-ghwebhook` will automatically wake up the agent via Webhook if CI fails or CodeRabbit submits reviews.
