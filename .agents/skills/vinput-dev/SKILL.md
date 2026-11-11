---
name: vinput-dev
description: "fcitx5-vinput 主开发路由与状态机执行引擎 (Master development router, S0-S4 state machine, playbooks, and C++20 architectural principles). Use when planning or implementing features, bug fixes, refactoring, or state machine routing. Command: /vinput-dev."
---

# fcitx5-vinput Development & State Machine Engine

Master development routing and lifecycle orchestration engine for `fcitx5-vinput`.

---

## 1. Complexity Gates & The Macro State Machine (`S0 -> S4`)

Every code modification proceeds through a state machine anchored in GitHub (PCB). The Main Agent is the **Single Writer** for the Git worktree.

### Complexity Gates: Fast-Path vs. Full-Path
- **Fast-Path (Single-module, low blast radius)**: Local logic within one module, CLI text formatting, pure documentation, or self-contained tests. Skip S0/S1 sub-issue slicing; proceed directly to Pre-Code Draft PR $\to$ S2 atomic loop.
- **Full-Path (High blast radius, cross-process, or schema risk)**: Mandatory when touching PipeWire RT audio callbacks (`src/daemon/audio/`), D-Bus signatures, Three-stream IPC protocols, configuration schema migrations (`ConfigMigration`), or cross-module refactors. Must complete S0 (Explore) and S1 (Slice).

### The Five-Stage State Machine

| State | Role & Trigger | Core Action & Execution Carrier | Handshake / Exit Gate |
| :--- | :--- | :--- | :--- |
| **`S0: EXPLORE`** | Grounding & Empirical Fork | • Dispatch `Grounding Scout` subagent for high-noise code/doc search.<br>• Run `/tmp` disposable prototype/scripts for empirical verification.<br>• Grilling on architecture/product trade-offs. | **[HITL]** Maintainer confirms proposed design; conclusion recorded in Issue. |
| **`S1: SLICE`** | Vertical Slicing & Task Prep | • Prefactor First: isolate structural preparation from behavioral change.<br>• Decompose into sub-issues with native `blocked_by` dependencies. | Top unblocked child issue marked `ready-for-agent`. |
| **`S2: TRACK_LOOP`** | Single-Writer Atomic Coding | • Push empty commit and open Pre-Code Draft PR with all `- [ ]`.<br>• Follow `Feature`, `Bug-Fix`, or `Refactoring` track invariants.<br>• Run `<0.3s` local gate (`mise run check:changed`).<br>• Push commit and check off `- [x]` in lockstep.<br>• Detect Scrap friction: revert and redesign if $\ge 2$ ad-hoc workarounds emerge. | All Draft PR `- [ ]` tasks completed (`- [x]`). |
| **`S2.5: TRIAGE_GATE`** | Anti-Slop & Duplication Audit | • Run `mise run check:dup` (`jscpd`).<br>• Triage clones: Type A (AI procedural slop) vs Type B (domain boilerplate). | **[HITL]** Maintainer confirms extraction or retention plan. |
| **`S3: GATE_YIELD`** | Dual-Axis Review & Yield | • Dispatch parallel read-only subagents for dual-axis review (Spec + Standards).<br>• Run `gh pr ready` to trigger full remote matrix CI and bots.<br>• Output PR/CI URLs and concise briefing. | **[HALT]** Yield control immediately. Zero polling. |
| **`S4: WEBHOOK_CLOSED`** | Event-Driven Wakeup & Merge | • Woken by `.pi/ghwebhook.json` on CI failure, Review Bot, or CI success.<br>• CI failure: dispatch `Forensics Investigator` subagent to diagnose.<br>• Review bot: triage into `[FIX] / [DISMISS] / [ASK]`.<br>• CI success: squash-merge, close child issue, advance parent state. | Branch deleted, main synced, next sub-issue unlocked. |

---

## 2. Subagent Delegation & Context Isolation Slots

To prevent context window bloat and eliminate self-defense review bias, the Main Agent dispatches subagents exclusively through **four standard slots**.

### Canonical Rules for Subagents
1. **Single Writer**: The Main Agent is the sole writer of the worktree and branch. Subagents are read-only, except `Prototype Runner` which is restricted to `/tmp/`.
2. **Fresh by Default**: Never resume an old subagent session. Always dispatch a fresh subagent with consolidated scope.
3. **Least Context Ingestion (Input)**: Never pass conversational history. Inject only the 4-tuple: $\langle \text{Role}, \text{Scope}, \text{Artifacts / Context Pointers}, \text{Schema} \rangle$.
4. **Synthesized Egress (Output)**: Discard the subagent's intermediate tokens upon process termination. The Main Agent receives only a structured, concise verdict (< 300 words).

### The Four Slots

| Slot | Source | Trigger | Injected Context (Input) | Synthesized Result (Output) |
| :--- | :--- | :--- | :--- | :--- |
| **`Grounding Scout`** | `how` / `why` | S0 Exploration; complex multi-file search or historical regression analysis. | Target file paths, symbols, research questions, search tool grants (`git log -S`, `context7`, `exa`). | 4-part summary: How it works, Why it is this way, Invariants to preserve, Key pointers (`file:line`). |
| **`Prototype Runner`** | Empirical Fork | S0 Exploration; uncertain protocol, timing, or state machine behavior. | Premise to test, sandbox path (`/tmp/vinput-proto-*`), verification criteria. | Empirical verdict: Premise tested, command run, measured output, and conclusion. |
| **`Forensics Investigator`** | `diagnose` | Bug-Fix track or S4 `ci-failure`; coredumps, GDB traces, CI failure logs. | Run ID or crash dump path, failure symptom. | Root cause report: Symptom to root cause mechanism, eliminated hypotheses, minimal fix seam. |
| **`Dual-Axis Reviewer`** | `code-review` | S3 Gate before `gh pr ready`. Two parallel fresh read-only subagents. | Subagent A: `git diff origin/main...HEAD` + Issue spec.<br>Subagent B: `git diff origin/main...HEAD` + `CODING_STANDARDS.md`. | Side-by-side reports under `## Spec` and `## Standards` (< 400 words each). No reranking. |

---

## 3. Scenario Playbooks

Match task intent to the dedicated scenario playbook:

| Playbook | Matching Intent & Triggers | Target File |
| :--- | :--- | :--- |
| **Feature** | New functionality, added behaviors, external contract additions. | [`playbooks/feature.md`](playbooks/feature.md) |
| **Bug Fix** | Correcting broken behavior, crashes, regressions, or audio artifacts. | [`playbooks/bug-fix.md`](playbooks/bug-fix.md) |
| **Refactoring** | Restructuring code with 100% behavior preservation, removing dead code. | [`playbooks/refactoring.md`](playbooks/refactoring.md) |
| **Audio Forensics** | PipeWire capture drops, XRuns, initial syllable clipping, daemon crashes. | [`playbooks/audio-forensics.md`](playbooks/audio-forensics.md) |
| **Packaging & CI** | Remote matrix validation, Dry Runs, SemVer release tagging. | [`playbooks/packaging-ci.md`](playbooks/packaging-ci.md) |
| **Fork Contribution** | Contributing from external forks, upstream sync, fork hygiene. | [`playbooks/fork-contribution.md`](playbooks/fork-contribution.md) |

---

## 4. Architectural Principles (Disclosed Reference)

Consult these authoritative principles when entering the relevant domain:

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
