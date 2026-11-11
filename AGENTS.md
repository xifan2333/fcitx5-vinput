# fcitx5-vinput Agent Guide

Air Traffic Control (ATC) steering guide, operational boundaries, and state-machine router for `fcitx5-vinput`.

> **Language Adaptation Directive**:
> Respond in the user's conversational language (e.g. Chinese for Chinese prompts, English for English prompts), regardless of the language of repository steering documents.

---

## 1. Subsystems & Module Boundaries

`fcitx5-vinput` provides local and cloud voice input across five distinct architectural layers:

- `src/common/`: Shared domain types, configuration (`glaze`), RAII wrappers, and D-Bus interfaces.
- `src/daemon/`: Core service (`vinput-daemon`), PipeWire audio capture, ASR inference, and D-Bus server (`org.fcitx.Vinput`).
- `src/addon/`: Fcitx5 input method plugin (Qt/C++, hotkeys, push-to-talk, D-Bus client).
- `src/cli/`: Standalone CLI (`vinput`) for recording, device inspection, and profile management.
- `src/gui/`: Standalone Qt 6 configuration GUI (`vinput-gui`).

---

## 2. Operational Invariants

1. **Branch Workflow**: Develop on dedicated feature branches via Pre-Code Draft PRs. Commit pure documentation and agent skills directly to `main` with `[skip ci]`.
2. **Draft PR Pre-Creation**: Push an empty commit and open a Draft PR with all checklist items unchecked (`- [ ]`) before writing functional code.
3. **Atomic Task Commits**: Map each commit to exactly one Draft PR checklist item. Push and check off `- [x]` in lockstep.
4. **CI-Only Compilation**: Maintainer (`xifan2333`) environment skips local compilation (`mise run build-debug` prohibited). Validate exclusively via remote CI (`ci.yml`, `release.yml`).
5. **Local Pre-Commit Gate**: Pass `mise run check:changed` (< 0.3s) before every push.
6. **Fork Fast-Forward Safety**: Push to contributor forks via fast-forward only; force-pushing to contributor forks is blocked by `hk.pkl`.
7. **Micro-Slicing**: Keep functional code diffs < 300 lines. Split larger epics into stacked PRs.
8. **Dialectical Bot Triage**: Route automated review comments (CodeRabbit, Greptile) through `/vinput-review` using `[FIX] / [DISMISS] / [ASK]` before modifying code.
9. **Non-Blocking Handover**: On `gh pr ready`, report PR and CI run URLs, then halt. Never run polling loops (`gh run watch` or sleep).
10. **Proactive Quota Cancellation**: Cancel stale in-progress runs via `gh run cancel` before pushing a new HEAD. Preserve runs on `main`.
11. **AI Attribution**: Append `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <ver>.*` to PR bodies and automated review fixes.

---

## 3. Complexity Gates & State Machine (`S0 -> S4`)

### Complexity Gates
- **Fast-Path (Single module, localized scope)**: Pure CLI text formatting, documentation, or self-contained logic within one module. Skip sub-issue slicing; proceed directly to Pre-Code Draft PR $\to$ S2 loop.
- **Full-Path (High blast-radius, cross-process, or schema risk)**: Mandatory when modifying PipeWire RT audio capture (`src/daemon/audio/`), D-Bus signatures, Three-stream IPC, configuration schema (`ConfigMigration`), or cross-module interfaces. Requires completing S0 and S1.

### State Transitions

| State | Role & Trigger | Action & Execution Carrier | Handshake & Exit Criteria |
| :--- | :--- | :--- | :--- |
| **`S0: EXPLORE`** | Grounding & Empirical Fork | • Run `Grounding Scout` subagent for high-noise searches.<br>• Run `/tmp` disposable prototype to settle behavioral forks. | **[HITL]** Maintainer confirms proposed design. Record conclusion in Issue. |
| **`S1: SLICE`** | Vertical Slicing & Task Prep | • Prefactor First: separate structural prep from feature logic.<br>• Create sub-issues with native `blocked_by` dependencies. | Top unblocked child issue marked `ready-for-agent`. |
| **`S2: TRACK_LOOP`** | Single-Writer Atomic Coding | • Open Pre-Code Draft PR with all `- [ ]`.<br>• Advance topmost `- [ ]` per Feature/Bug-Fix/Refactor playbook.<br>• Run `<0.3s` gate (`mise run check:changed`).<br>• Push commit and check off `- [x]` in lockstep.<br>• Scrap Detector: abort and redesign if $\ge 2$ ad-hoc workarounds emerge. | All Draft PR tasks checked off (`- [x]`). |
| **`S2.5: TRIAGE_GATE`** | Anti-Slop Duplication Audit | • Run `mise run check:dup` (`jscpd`).<br>• Classify clones: Type A (procedural slop) vs Type B (domain boilerplate). | **[HITL]** Maintainer confirms abstraction or retention plan. |
| **`S3: GATE_YIELD`** | Dual-Axis Review & Yield | • Dispatch parallel subagents for Spec + Standards review.<br>• Mark ready: `gh pr ready`.<br>• Output PR/CI URLs and concise summary. | **[HALT]** Yield control immediately. Zero polling. |
| **`S4: WEBHOOK_CLOSED`** | Event-Driven Wakeup & Merge | • Woken by `.pi/ghwebhook.json` on CI failure, Bot review, or CI success.<br>• CI failure: dispatch `Forensics Investigator` subagent.<br>• Review bot: triage into `[FIX] / [DISMISS] / [ASK]`.<br>• CI success: squash-merge, close child issue, advance parent state. | Branch deleted, main synced, next sub-issue unlocked. |

---

## 4. Local Commands

```bash
mise run check:plan       # Preview execution plan and linters
mise run check:changed    # Validate touched files (< 0.3s)
mise run fix              # Auto-format modified files (clang-format, cmake-format, ruff)
mise run check:dup        # Audit code duplication across src/
```

---

## 5. Navigation Pointers

Context pointers to on-demand reference skills (front-loaded triggers, bilingual):

| Skill | Trigger & Scope (中英双语触发) | When to Consult |
| :--- | :--- | :--- |
| **`vinput-dev`** | `Develop C++20 features, bug fixes, and refactorings across S0-S4 state machine. 触发：开发新功能、修缺陷、重构、/vinput-dev` | Planning, branching, and implementing within S0–S2. |
| **`/vinput-review`** | `Review diffs along dual axes (Spec + Standards) and triage bot comments. 触发：代码审查、双轴评审、审查机器人对质、/vinput-review` | S3 pre-ready gate or when review comments arrive. |
| **`/vinput-diagnose`** | `Diagnose crashes, XRuns, GDB traces, and PipeWire faults without compilation. 触发：崩溃排障、断音丢帧、GDB取证、/vinput-diagnose` | S0/S4 investigating crashes, deadlocks, or XRuns. |
| **`/vinput-retro`** | `Encode post-session lessons into ast-grep rules, hooks, and standards refinement. 触发：会话复盘、规则免疫、/vinput-retro` | S4 post-merge or after fixing repeated CI breaks. |
| **[`CODING_STANDARDS.md`](CODING_STANDARDS.md)** | `Audit architectural judgement: deep modules, seam discipline, RT safety, and Fowler smells` | Review phase only; never load during active implementation. |
| **[`GLOSSARY.md`](GLOSSARY.md)** | `Domain vocabulary SSOT: three-stream subprocess model, ASR sessions, and IME routing` | Naming domain types, D-Bus methods, and PR titles. |
