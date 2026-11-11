---
name: vinput-dev
description: "Develop C++20 features, bug fixes, and refactorings across the S0-S4 execution track. 触发：开发新功能、修缺陷、重构、/vinput-dev。"
---

# Development Execution Track

Walk S0 through S4 in order. Each step ends on its completion criterion.

Enter at S0 when the change touches real-time capture under
`src/daemon/audio/`, a D-Bus signature, three-stream IPC, a configuration
migration, or a cross-module interface. Enter at S1 for a single-module change,
documentation, or self-contained tests.

## S0 Explore

- Locate the Issue and read its requirements and acceptance boundary.
- Dispatch read-only reconnaissance over call chains, upstream practice, and
  prior commits (`references/execution-adapters.md` section 1).
- Where behaviour is unsettled, settle it with a disposable prototype under
  `/tmp/vinput-proto-*`.
- File report 1 from `references/reporting.md`.

Completion criterion: maintainer confirms the proposed design in chat.

## S1 Slice

- Split full-path work into child Issues wired by native dependencies.
- Create `<type>/issue-<id>-<slug>`, push an empty commit.
- Open a Draft PR whose checklist is entirely unchecked.
- File report 2 from `references/reporting.md`.

Completion criterion: Draft PR active, checklist all unchecked.

## S2 Atomic Implementation

For each checklist item, topmost first:

- Implement only the current item, smallest complete diff.
- Run `mise run check:changed`.
- Commit, push, check the item off in the Draft PR body.
- File report 3 from `references/reporting.md`, then continue to the next item.
- On two or more special-case branches, or a broken interface assumption, file
  report 4, run `git reset`, and stop.

Completion criterion: every checklist item checked.

## S2.5 Duplication Gate

- Run `mise run check:dup`.
- File report 5 from `references/reporting.md`.

Completion criterion: maintainer confirms extract or retain.

## S3 Handover

- Dispatch two isolated read-only reviews (`references/execution-adapters.md`
  section 2).
- Run `gh pr ready`.
- File report 6 from `references/reporting.md`.

Completion criterion: control yielded immediately, zero polling.

## S4 Event Closure

Branch on the wake event:

- CI failure: establish the cause via `vinput-diagnose`, then fix.
- Review bot comment: triage through `vinput-review` into `[FIX] / [DISMISS] / [ASK]`.
- CI green: squash-merge, sync `main`, verify the Issue closed.

File report 7 from `references/reporting.md`.

Completion criterion: branch deleted, `main` synced, Issue closed.

## References

- `references/lifecycle.md` — Issue-to-PR traceability contract
- `references/state-machine.md` — state, role, and access boundaries
- `references/reporting.md` — disclosure templates
- `references/execution-adapters.md` — capability contracts and host adapters
- `playbooks/` — one file per task kind
- `principles/` — one file per design decision
