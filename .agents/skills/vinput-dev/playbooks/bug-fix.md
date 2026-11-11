# Playbook: Bug Fix Development

The canonical execution workflow for diagnosing and repairing unintended or broken behaviors in `fcitx5-vinput`.

> **Execution Contract**:
> Be scientific. Every fix must trace to concrete runtime or diagnostic evidence. Belt-and-suspenders patches ("might help") are strictly prohibited. The Main Agent acts as the Single Writer.

---

## Steps

1. **Forensic Evidence & Root Cause (S0: EXPLORE)**:
   - If a crash, XRun, or core dump is involved, consult `/vinput-diagnose` (inspect coredump, GDB stack traces, or symbol offsets).
   - Binary-search the cause: trace the failure back to the first place the system deviated from valid state.
   - **Attack the Premise**: Write down the working hypothesis and verify it against evidence before writing code.
   - Identify the single shared layer where the root cause lives. Never scatter caller-side defensive null checks when the state is illegitimate.

2. **Failing Reproduction Test First (Red -> Green)**:
   - Where local unit testing exists (pure logic, string parsing, state transition), construct a failing test case before touching production code.
   - Verify the test fails for the expected reason (`Red`).

3. **Pre-Create Draft PR**:
   - Push an empty commit and open a Draft PR with all tasks unchecked:
     ```bash
     git commit --allow-empty -m "chore: initialize fix for #<issue-id>"
     git push -u origin <branch-name>
     gh pr create --draft \
       --title "fix(<scope>): <concise description> (#<issue-id>)" \
       --body "Fixes #<issue-id>

### Implementation Tasks
- [ ] 1. Failing test reproduction / forensic evidence pin
- [ ] 2. Root cause fix in shared layer
- [ ] 3. Regression verification

*AI-assisted — Tool: <tool>; model: <model>; version: <ver>.*"
     ```

4. **Single-Item Atomic Loop (S2: TRACK_LOOP)**:
   - Fix the root cause in the shared component with the minimal working diff.
   - Verify the reproduction test now passes (`Green`).
   - Run local sub-second gate:
     ```bash
     mise run check:changed
     ```
   - Push atomic commit and update Draft PR checklist (`- [x]`) in lockstep.

5. **Self-Review & Gate Yield (S3: GATE_YIELD)**:
   - Run `mise run check:dup`.
   - Dispatch Dual-Axis Reviewer via `/vinput-review`.
   - Mark ready: `gh pr ready <pr-id>`.
   - Yield control immediately with PR and CI links.
