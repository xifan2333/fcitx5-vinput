# fcitx5-vinput Agent Guide

Air Traffic Control (ATC) steering guide, operational red lines, and routing index for AI coding agents on `fcitx5-vinput`.

> **Language Adaptation Directive**:
> Respond in the language used in the current conversation or user prompt (e.g., respond in Chinese if the user prompts in Chinese, English if in English). Do NOT default to English simply because this document or other steering files are written in English.

---

## 1. Project Overview & Boundaries

`fcitx5-vinput` is an offline and cloud voice input system for Fcitx5 providing local (sherpa-onnx) and cloud ASR, LLM post-processing, and packaging.

- `src/addon/`: Fcitx5 input method addon (Qt/C++, hotkey triggers, push-to-talk, D-Bus client).
- `src/daemon/`: Core daemon (`vinput-daemon`, PipeWire audio capture, ASR inference, D-Bus service `org.fcitx.Vinput`).
- `src/cli/`: Standalone `vinput` CLI for recording, profile switching, and status inspection.
- `src/common/`: Shared domain types, configuration structs (`glaze`), and D-Bus XML interfaces.
- `src/gui/`: Standalone Qt 6 configuration GUI (`vinput-gui`).

---

## 2. Operational Red Lines

1. **No Direct Main Commits**: Develop exclusively via feature branches and Draft PRs. Documentation and agent skills (`AGENTS.md`, `.agents/`) may commit directly to `main` with `[skip ci]`.
2. **Draft PR Pre-Creation**: Open a Draft PR with all checklist items unchecked (`- [ ]`) before writing implementation code.
3. **Atomic Task Commits**: Each commit must map to exactly one task in the PR checklist. Push and check off `- [x]` in lockstep.
4. **Zero Local Compilation for Maintainer**: In the maintainer environment (`xifan2333`), local compilation is prohibited (`mise run build-debug` forbidden). Rely exclusively on remote CI (`ci.yml`, `release.yml`).
5. **Local Pre-Commit Gate**: Always run `mise run check:changed` (< 0.3s) before pushing.
6. **No Force-Push to Contributor Forks**: Never run `git push -f` against external contributor branches or personal forks.
7. **PR Micro-Slicing**: Keep functional code changes < 300 lines. Decompose larger efforts into stacked PRs.
8. **Dialectical Review Bot Triage**: Never accept review bot comments (CodeRabbit, Greptile) uncritically. Use `/vinput-review` with the three-way `[FIX] / [DISMISS] / [ASK]` protocol to formulate grounded evidence.
9. **Non-Blocking Handover**: Never run blocking CI polling loops (`gh run watch` or sleep). Output PR and run URLs, then yield control.
10. **Proactive CI Cancellation**: Cancel stale in-progress runs (`gh run cancel`) before pushing follow-up commits. Never cancel runs on `main`.
11. **AI Attribution**: Append `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*` to PR descriptions and review fixes.

---

## 3. Complexity Dual-Track & State Machine (Everything is a State Machine)

### Complexity Gates: Fast-Path vs Full-Path
- **Fast-Path (Single-module, well-defined scope)**: Local changes within a single component, CLI text formatting, pure documentation, or self-contained bug fixes. Skip multi-issue slicing; proceed directly to Pre-Code Draft PR $\to$ atomic track loop.
- **Full-Path (High blast-radius, cross-process, or schema risk)**: Mandatory when touching PipeWire RT audio capture (`src/daemon/audio/`), D-Bus interface signatures, Three-stream IPC protocols, configuration schema migrations (`ConfigMigration`), or cross-module refactors. Must execute S0 (Explore) and S1 (Slice).

### The Five-Stage State Machine (`S0 -> S4`)

| State | Role & Trigger | Core Action & Execution Carrier | Handshake / Exit Gate |
| :--- | :--- | :--- | :--- |
| **`S0: EXPLORE`** | Grounding & Empirical Fork | • Dispatch `Grounding Scout` subagent for high-noise code/doc search.<br>• Run `/tmp` disposable prototype/scripts for empirical verification.<br>• Grilling on architecture/product trade-offs. | **[HITL]** Maintainer confirms proposed design; conclusion recorded in Issue. |
| **`S1: SLICE`** | Vertical Slicing & Task Prep | • Prefactor First: isolate structural preparation from behavioral change.<br>• Decompose into sub-issues with native `blocked_by` dependencies. | Top unblocked child issue marked `ready-for-agent`. |
| **`S2: TRACK_LOOP`** | Single-Writer Atomic Coding | • Push empty commit and open Pre-Code Draft PR with all `- [ ]`.<br>• Follow `Feature`, `Bug-Fix`, or `Refactoring` track invariants.<br>• Run `<0.3s` local gate (`mise run check:changed`).<br>• Push commit and check off `- [x]` in lockstep.<br>• Detect Scrap friction: revert and redesign if $\ge 2$ ad-hoc workarounds emerge. | All Draft PR `- [ ]` tasks completed (`- [x]`). |
| **`S2.5: TRIAGE_GATE`** | Anti-Slop & Duplication Audit | • Run `mise run check:dup` (`jscpd`).<br>• Triage clones: Type A (AI procedural slop) vs Type B (domain boilerplate). | **[HITL]** Maintainer confirms extraction or retention plan. |
| **`S3: GATE_YIELD`** | Dual-Axis Review & Yield | • Dispatch parallel read-only subagents for dual-axis review (Spec + Standards).<br>• Run `gh pr ready` to trigger full remote matrix CI and bots.<br>• Output PR/CI URLs and concise briefing. | **[HALT]** Yield control immediately. Zero polling. |
| **`S4: WEBHOOK_CLOSED`** | Event-Driven Wakeup & Merge | • Woken by `.pi/ghwebhook.json` on CI failure, Review Bot, or CI success.<br>• CI failure: dispatch `Forensics Investigator` subagent to diagnose.<br>• Review bot: triage into `[FIX] / [DISMISS] / [ASK]`.<br>• CI success: squash-merge, close child issue, advance parent state. | Branch deleted, main synced, next sub-issue unlocked. |

---

## 4. Core Commands & Quality Gates

```bash
# Local Quality Gates (Sub-second static checks, zero compilation)
mise run check:plan       # Preview execution plan and linters
mise run check:changed    # Validate only touched files (safe, fast, < 0.3s)
mise run fix              # Auto-format modified files via hk (clang-format, cmake-format, ruff)
mise run check:dup        # Duplication detection across src/

# Remote CI Validation
gh workflow run ci.yml && gh run watch                         # Standard PR CI
gh workflow run release.yml --ref <branch> && gh run watch     # Remote multi-arch matrix dry run

# Cancel Stale CI Runs
gh run list --branch $(git branch --show-current) --status in_progress --json databaseId,headSha --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" | xargs -r -n 1 gh run cancel
```

---

## 5. Repository Harness & Navigation Pointers

| Skill / Command | Intent & Trigger (中英双语触发) | When to Consult |
| :--- | :--- | :--- |
| **`vinput-dev`** | `fcitx5-vinput 主开发路由与状态机执行引擎 (Master development router, S0-S4 state machine, playbooks, C++20 principles)` | Implementation phase; planning and executing Feature, Bug-Fix, or Refactoring tracks. |
| **`/vinput-review`** | `fcitx5-vinput 离线双轴独立审查与 Review Bot 辩证分流技能 (Offline dual-axis static diff review: Spec vs Standards, plus Review Bot [FIX]/[DISMISS]/[ASK] triage)` | Before marking PR ready, before pushing, or when responding to bot comments. |
| **`/vinput-diagnose`** | `fcitx5-vinput 运行时排障与 GDB/逆向二进制取证引擎 (Runtime crash, deadlock, XRun, and ELF inspection via GDB, coredumpctl, strace, readelf, addr2line without local compilation)` | Diagnosing crashes, SIGSEGV, hangs, buffer drops, or daemon defects. |
| **`/vinput-retro`** | `fcitx5-vinput 会话复盘与规则免疫自迭代引擎 (Retrospective engine to encode lessons into ast-grep rules, pointers, and standards refinement after CI runs or PR merges)` | After CI failures, after PR merges, or during session retrospectives. |
| **[`CODING_STANDARDS.md`](CODING_STANDARDS.md)** | Architectural judgement standards and Fowler smells | Review phase only; never load during active implementation. |
| **[`GLOSSARY.md`](GLOSSARY.md)** | Domain modeling and ubiquitous language SSOT | Naming domain entities, D-Bus methods, and PR titles. |
