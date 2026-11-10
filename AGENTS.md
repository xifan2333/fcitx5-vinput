# fcitx5-vinput Agent Guide

Progressive operational guide, workflow red lines, and routing index for AI coding agents on `fcitx5-vinput`.

---

## 1. Project Overview & Architecture

`fcitx5-vinput` is an offline/cloud voice input system for Fcitx5 providing local (sherpa-onnx) and cloud ASR, LLM post-processing, and cross-distro packaging.

- `src/addon/`: Fcitx5 input method addon (Qt/C++, hotkey triggers like `Alt_R` / `Shift_R`, push-to-talk, D-Bus client).
- `src/daemon/`: Core daemon (`vinput-daemon`, PipeWire audio capture, sherpa-onnx inference, cloud ASR engines, LLM scene transformations, D-Bus service `org.fcitx.Vinput`).
- `src/cli/`: Standalone `vinput` CLI for manual recording, profile switching, and status inspection.
- `src/common/`: Shared types, configuration structs (`glaze`), isolated ConfigMigration engine (`config/config_migration.cpp`), D-Bus XML interfaces.
- `src/gui/`: Standalone Qt 6 configuration GUI (`vinput-gui`).
- `po/` & `i18n/`: Translations (Gettext and Qt `.ts` / `.qm`).

---

## 2. Operational Red Lines (Workflow & PR Protocol)

1. **No Direct Main Commits for Code**: Never push implementation code, build configurations, or tests directly to `main`. Always develop via feature branches and Draft PRs.
   *Fast-track Exception*: Pure documentation, agent guides/skills (`AGENTS.md`, `CLAUDE.md`, `.agents/`), and non-functional text edits are exempt and may be committed directly to `main` with `[skip ci]`.
2. **Draft PR Pre-Creation**: Open a Draft PR with ALL checklist items UNCHECKED (`- [ ]`) before writing implementation code.
3. **No Multi-Task Lump Commits & Progressive Sync**: For changes developed via Draft PRs, every commit must map to exactly one `- [ ]` task in the PR checklist. After passing local quality gates (`mise run check:changed`), commit atomically, push, and mark `- [x]` in lockstep.
4. **Pre-Commit Quality Gate**: Run `mise run check:changed` or `hk run check --safe` before pushing.
5. **Hardware-Adaptive & User-Specific Compilation**:
   - If the GitHub user is `xifan2333` (maintainer environment), **local compilation is strictly prohibited (never run `mise run build-debug`)**; rely exclusively on CI (`ci.yml` / `release.yml`).
6. **No Force-Pushing to Contributor Forks**: Never `git push -f` to external contributors' personal forks or PR branches.
7. **PR Micro-Slicing**: Keep functional code changes < 300 lines (excluding tests/generated docs); decompose complex epics into stacked micro-PRs.
8. **AI Attribution & Disclosure**: Append `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*` to PR descriptions, review fixes, and substantive comments.
9. **Dialectical Review Bot Triage**: Never treat automated review bots (CodeRabbit, Greptile) as unquestioned instructions. Critically evaluate findings against C++20 standards and project architecture. Report analysis to the human maintainer in chat before implementing fixes.
10. **Non-Blocking Handover (No CI Polling)**: Never execute blocking polling loops (`gh run watch` or sleep loops). Immediately after running `gh pr ready`, output the PR and Actions run URLs and yield control.
11. **Issue-PR Lifecycle Complete Closure**: After squash-merging a PR, switch back to `main`, pull latest, and verify the originating issue is in the `CLOSED` state (manually close via `gh issue close <id>` if needed).
12. **Proactive CI Quota Cancellation**: Cancel stale in-progress runs (`gh run cancel`) before pushing follow-up fixes or amending commits. Never cancel runs on `main`.

---

## 3. Code Standards & Architecture Enforcement

> **Implementation vs. Review Separation**:
> Implementation agents focus on minimal, clean solutions. Detailed coding standards and Fowler code smells are evaluated during review against `CODING_STANDARDS.md`.

- **Coding Standards (SSOT)**: See [`CODING_STANDARDS.md`](CODING_STANDARDS.md) for complete rules governing real-time audio safety, module depth, SSOT common infrastructure, cognitive complexity ($\le 15$), C++20 memory models, zero-diagnostic suppression, and pristine 3rdparty governance.
- **Domain Modeling & Ubiquitous Language**: See [`GLOSSARY.md`](GLOSSARY.md). When naming domain concepts (in issue titles, function names, tests), strictly use the vocabulary defined in the glossary.

---

## 4. Core Commands & Quality Gates

```bash
# Local Quality Gates (Sub-second execution)
mise run check:plan       # Preview execution plan and linters
mise run check:changed    # Validate only touched files (safe, fast, < 0.3s)
mise run check:ast        # Audit C++ AST structural anti-patterns via ast-grep
mise run check:dup        # Analyze code duplication across src/ (jscpd)
mise run check:complexity # Audit cyclomatic & cognitive complexity (CCN <= 15, CogC <= 15)
mise run check:typos      # Audit identifier and code spelling integrity (typos)
mise run check:cmake      # Validate CMake formatting (cmake-format)
mise run fix              # Auto-format all files via hk (clang-format, cmake-format, ruff, shfmt)

# Remote CI Validation
gh workflow run ci.yml && gh run watch                         # Standard PR CI
gh workflow run release.yml --ref <branch> && gh run watch     # Remote multi-arch matrix dry run

# Cancel Stale CI Runs
gh run list --branch $(git branch --show-current) --status in_progress --json databaseId,headSha --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" | xargs -r -n 1 gh run cancel
```

---

## 5. Navigation Pointers (Progressive Disclosure)

| Topic | Pointer | When to Consult |
| :--- | :--- | :--- |
| **Code Standards & Smells** | [`CODING_STANDARDS.md`](CODING_STANDARDS.md) | Writing C++20 code, reviewing PRs, checking linter rules |
| **Domain Glossary** | [`GLOSSARY.md`](GLOSSARY.md) | Naming domain primitives, structs, D-Bus methods, and tests |
| **Domain Docs Layout** | [`docs/agents/domain.md`](docs/agents/domain.md) | Exploring single-context vs multi-context documentation |
| **Issue Tracker Protocol** | [`docs/agents/issue-tracker.md`](docs/agents/issue-tracker.md) | Reading/creating GitHub issues, managing dependencies, wayfinding |
| **Triage Labels** | [`docs/agents/triage-labels.md`](docs/agents/triage-labels.md) | Mapping canonical triage roles to issue labels |
| **Issue & PR Lifecycle** | [issue-pr-workflow.md](.agents/skills/vinput-dev/references/issue-pr-workflow.md) | Detailed branch setup, PR checklists, and bot triage |
| **Compilation & Packaging** | [compilation-ci.md](.agents/skills/vinput-dev/references/compilation-ci.md) | Matrix builds, packaging channels, and SemVer release rules |
| **Audio & PipeWire Tracing**| [audio-debugging.md](.agents/skills/vinput-dev/references/audio-debugging.md) | Troubleshooting audio capture, SPA ringbuffers, and XRuns |
| **Multi-Repo Ecosystem** | [ecosystem.md](.agents/skills/vinput-dev/references/ecosystem.md) | Cross-repo links: vinput-registry, aur-auto, flatpak-auto |
| **C ABI Hard Boundaries** | [c-abi-boundaries.md](.agents/skills/vinput-dev/references/c-abi-boundaries.md) | Interfacing with PipeWire, sd-bus, POSIX, and libarchive |
| **Dependency Governance** | [dependency-spec.md](.agents/skills/vinput-dev/references/dependency-spec.md) | Tier 1 host packages vs. Tier 2 `3rdparty/` pristine vendoring |
| **Fcitx5 Addon Pillar** | Context7: `/fcitx/fcitx5` | Modifying `src/addon/` key handling and configuration |
| **Speech Engine Pillar** | Context7: `/k2-fsa/sherpa-onnx` | Modifying `src/daemon/asr/` models, recognizers, and VAD |
| **PipeWire Capture Pillar** | Context7: `/pipewire/pipewire` | Modifying `src/daemon/audio/` capture loops and streams |
| **CLI Parser Pillar** | Context7: `/cliutils/cli11` | Modifying `src/cli/` subcommand trees and validators |
| **Qt 6 GUI Pillar** | Context7: `/websites/doc_qt_io_qt-6` | Modifying `src/gui/` widgets, models, and views |
