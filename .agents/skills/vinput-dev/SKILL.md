---
name: vinput-dev
description: "Develop C++20 features, bug fixes, and refactorings across S0-S4 state machine. 触发：开发新功能、修缺陷、重构、/vinput-dev。"
---

# Development & State Machine Router

Master execution router, state-machine transitions, and playbook dispatcher for `fcitx5-vinput`.

---

## 1. Complexity Gates & S0–S4 State Machine

The Main Agent is the **Single Writer** for the Git worktree. All work proceeds through discrete states:

### Complexity Gates
- **Fast-Path**: Single-module changes, CLI formatting, or self-contained tests. Skip S0/S1 sub-issue slicing; proceed directly to Pre-Code Draft PR $\to$ S2 atomic loop.
- **Full-Path**: PipeWire RT audio callbacks (`src/daemon/audio/`), D-Bus signatures, Three-stream IPC, configuration schema migrations (`ConfigMigration`), or cross-module interfaces. Mandatory S0 and S1 execution before coding.

### States and Completion Criteria

1. **`S0: EXPLORE` (Grounding & Empirical Verification)**
   - *Action*: Dispatch `Grounding Scout` subagent for high-noise searches. Run `/tmp` disposable prototypes to settle behavioral or protocol forks.
   - *Completion Criterion*: Proposed design and empirical findings verified. Maintainer confirms selection in chat. Conclusion recorded in GitHub Issue.
2. **`S1: SLICE` (Vertical Slicing & Task Prep)**
   - *Action*: Apply Prefactor First. Split structural refactoring from behavioral addition. Decompose into sub-issues linked by native `blocked_by` dependencies.
   - *Completion Criterion*: Root child issue is unblocked (`blocked_by == 0`) and labelled `ready-for-agent`.
3. **`S2: TRACK_LOOP` (Single-Writer Atomic Coding)**
   - *Action*: Open Pre-Code Draft PR with all `- [ ]` tasks. Advance topmost task per playbook. Run `mise run check:changed` (< 0.3s). Push atomic commit and check off `- [x]` in lockstep.
   - *Scrap Detector*: If implementation requires $\ge 2$ special-case branches or breaks initial interface assumptions, abort immediately. Run `git reset`, report friction, and redesign.
   - *Completion Criterion*: Every task in the Draft PR body is verified and checked off (`- [x]`).
4. **`S2.5: TRIAGE_GATE` (Duplication & Anti-Slop Audit)**
   - *Action*: Run `mise run check:dup`. Triage clone blocks into Type A (procedural slop) vs Type B (domain boilerplate).
   - *Completion Criterion*: Triage report delivered to maintainer. Extraction or retention confirmed.
5. **`S3: GATE_YIELD` (Dual-Axis Review & Yield)**
   - *Action*: Dispatch parallel read-only subagents for Spec + Standards review. Mark PR ready: `gh pr ready`. Output PR/CI URLs.
   - *Completion Criterion*: Control yielded immediately (HALT). Zero polling.
6. **`S4: WEBHOOK_CLOSED` (Asynchronous Event Loop & Merge)**
   - *Action*: Woken by `.pi/ghwebhook.json`. On CI failure: dispatch `Forensics Investigator`. On review comments: triage via `[FIX] / [DISMISS] / [ASK]`. On CI success: squash-merge.
   - *Completion Criterion*: PR squash-merged, remote branch deleted, local `main` synchronized, and child issue closed.

---

## 2. Subagent Delegation & Context Firewall

Protect the main context window from token exhaustion and eliminate self-defense review bias.

### Delegation Rules
1. **Single Writer**: The Main Agent retains exclusive write access to the worktree and branch. Subagents are strictly read-only, except `Prototype Runner` which is restricted to `/tmp/`.
2. **Fresh by Default**: Never resume an old subagent session. Always dispatch a fresh subagent with consolidated scope.
3. **Least Context Ingestion**: Never pass conversational history. Inject only the 4-tuple: $\langle \text{Role}, \text{Scope}, \text{Artifacts / Context Pointers}, \text{Schema} \rangle$.
4. **Synthesized Egress**: Discard the subagent's intermediate tokens upon process termination. The Main Agent receives only a structured, concise verdict (< 300 words).

### The Four Slots

| Slot | Source | Trigger | Injected Input (Pointers) | Synthesized Output (< 300 words) |
| :--- | :--- | :--- | :--- | :--- |
| **`Grounding Scout`** | `how` / `why` | S0 Exploration; complex multi-file search or historical regression analysis. | Target file paths, symbols, research questions, search tool grants (`git log -S`, `context7`, `exa`). | 4-part summary: How it works, Why it is this way, Invariants to preserve, Key pointers (`file:line`). |
| **`Prototype Runner`** | Empirical Fork | S0 Exploration; uncertain protocol, timing, or state machine behavior. | Premise to test, sandbox path (`/tmp/vinput-proto-*`), verification criteria. | Empirical verdict: Premise tested, command run, measured output, and conclusion. |
| **`Forensics Investigator`** | `diagnose` | Bug-Fix track or S4 `ci-failure`; coredumps, GDB traces, CI failure logs. | Run ID or crash dump path, failure symptom. | Root cause report: Symptom to root cause mechanism, eliminated hypotheses, minimal fix seam. |
| **`Dual-Axis Reviewer`** | `code-review` | S3 Gate before `gh pr ready`. Two parallel fresh read-only subagents. | Subagent A: `git diff origin/main...HEAD` + Issue spec.<br>Subagent B: `git diff origin/main...HEAD` + `CODING_STANDARDS.md`. | Side-by-side reports under `## Spec` and `## Standards` (< 400 words each). No reranking. |

---

## 3. Scenario Playbooks

Route the task to its dedicated scenario playbook:

- **[`playbooks/feature.md`](playbooks/feature.md)**: New functionality, added behaviors, external contract additions.
- **[`playbooks/bug-fix.md`](playbooks/bug-fix.md)**: Correcting broken behavior, crashes, regressions, or audio artifacts.
- **[`playbooks/refactoring.md`](playbooks/refactoring.md)**: Restructuring code with 100% behavior preservation, removing dead weight.
- **[`playbooks/audio-forensics.md`](playbooks/audio-forensics.md)**: PipeWire capture drops, XRuns, initial syllable clipping, daemon crashes.
- **[`playbooks/packaging-ci.md`](playbooks/packaging-ci.md)**: Remote matrix validation, Dry Runs, SemVer release tagging.
- **[`playbooks/fork-contribution.md`](playbooks/fork-contribution.md)**: Contributing from external forks, upstream sync, fork hygiene.

---

## 4. Architectural Principles

Consult authoritative engineering principles when making design decisions:

- **[`principles/laziness-protocol.md`](principles/laziness-protocol.md)**: Smallest complete change. Reuse canonical `src/common/` infrastructure (`glaze`, `spdlog`, `process_utils`, `mio`, `readerwriterqueue`). Deletion beats addition. Fix root cause once in shared code.
- **[`principles/c-abi-boundaries.md`](principles/c-abi-boundaries.md)**: Six canonical C ABI boundaries (PipeWire, sd-bus, libarchive, OpenSSL, POSIX, Fcitx5 addon). Defensive wrapper guidelines.
- **[`principles/dependency-spec.md`](principles/dependency-spec.md)**: Two-tier binary boundary (Tier 1 host shared libraries vs Tier 2 `3rdparty/` header-only). Pristine 3rdparty rule. Zero network downloads during build.
- **[`principles/module-boundaries.md`](principles/module-boundaries.md)**: Strict separation of common primitives (`src/common/`) from domain orchestration (`src/cli/`, `src/daemon/`, `src/addon/`, `src/gui/`). Cognitive complexity ceiling $\le 15$.
- **[`principles/ecosystem-routes.md`](principles/ecosystem-routes.md)**: Multi-repo ecosystem routing (`vinput-registry`, `aur-auto`, `flatpak-auto`).

---

## 5. Implementation vs Review Separation

- **During Implementation (S2)**: Focus strictly on minimal, clean code conforming to `principles/laziness-protocol.md`. Do not pre-load review standards into context during active coding.
- **During Review (S3)**: Audit the diff via `/vinput-review` strictly against [`CODING_STANDARDS.md`](../../CODING_STANDARDS.md) using isolated read-only subagents.

---

## 6. Upstream Pillar Grounding (Context7)

Query Context7 before modifying the five core pillars:
- **Fcitx5 Addon (`src/addon/`)**: Context7: `/fcitx/fcitx5`.
- **Speech Engine (`src/daemon/asr/`)**: Context7: `/k2-fsa/sherpa-onnx`.
- **PipeWire Audio (`src/daemon/audio/`)**: Context7: `/pipewire/pipewire`.
- **CLI Subcommands (`src/cli/`)**: Context7: `/cliutils/cli11`.
- **Qt 6 GUI (`src/gui/`)**: Context7: `/websites/doc_qt_io_qt-6`.
