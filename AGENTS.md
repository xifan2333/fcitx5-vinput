# fcitx5-vinput Agent Guide

Air Traffic Control (ATC) steering guide, operational red lines, and routing index for AI coding agents on `fcitx5-vinput`.

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
8. **Dialectical Review Bot Triage**: Never accept review bot comments (CodeRabbit, Greptile) uncritically. Use `/vinput-5w1h` to formulate grounded evidence.
9. **Non-Blocking Handover**: Never run blocking CI polling loops (`gh run watch` or sleep). Output PR and run URLs, then yield control.
10. **Proactive CI Cancellation**: Cancel stale in-progress runs (`gh run cancel`) before pushing follow-up commits. Never cancel runs on `main`.
11. **AI Attribution**: Append `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*` to PR descriptions and review fixes.

---

## 3. Core Commands & Quality Gates

```bash
# Local Quality Gates (Sub-second static checks, zero compilation)
mise run check:plan       # Preview execution plan and linters
mise run check:changed    # Validate only touched files (safe, fast, < 0.3s)
mise run fix              # Auto-format modified files via hk (clang-format, cmake-format, ruff)

# Remote CI Validation
gh workflow run ci.yml && gh run watch                         # Standard PR CI
gh workflow run release.yml --ref <branch> && gh run watch     # Remote multi-arch matrix dry run

# Cancel Stale CI Runs
gh run list --branch $(git branch --show-current) --status in_progress --json databaseId,headSha --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" | xargs -r -n 1 gh run cancel
```

---

## 4. Repository Harness & Navigation Pointers

| Skill / Command | Intent & Trigger | When to Consult |
| :--- | :--- | :--- |
| **`vinput-dev`** | Background development router and C++20 principles | Implementation phase; apply `principles/laziness-protocol.md` |
| **`/vinput-5w1h`** | Requirement triage, RFC proposals, and review bot triage | Before writing code or when responding to automated reviews |
| **`/vinput-diagnose`** | Crash, deadlock, XRun, and ELF/symbol forensics | Runtime faults, coredump analysis, GDB backtraces (zero compilation) |
| **`/vinput-review`** | Offline dual-axis static diff review (Spec + Standards) | Before marking PR ready; audits diff against `CODING_STANDARDS.md` |
| **`/vinput-retro`** | Post-session retrospective and self-evolution engine | After CI failures or PR merge; generates `.ast-grep` rules |
| **[`CODING_STANDARDS.md`](CODING_STANDARDS.md)** | Architectural judgement standards and Fowler smells | Review phase only; never load during active implementation |
| **[`GLOSSARY.md`](GLOSSARY.md)** | Domain modeling and ubiquitous language SSOT | Naming domain entities, D-Bus methods, and PR titles |
