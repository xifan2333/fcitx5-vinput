# Claude Code Guide

Compact index and operational quick-reference for Claude Code on `fcitx5-vinput`. For complete project architecture and detailed operational standards, see [AGENTS.md](AGENTS.md).

---

## 1. Core Commands & Quality Gates

```bash
# Local Quality Gate (Run before every commit and push - sub-second)
mise run check:plan       # Preview execution plan and linters
mise run check:changed    # Validate only touched files (safe, fast)
mise run check:dup        # Analyze code duplication across src/ (jscpd)
mise run check:complexity # Audit cyclomatic & cognitive complexity (CCN <= 15, CogC <= 15)
mise run fix              # Auto-format all modified files via hk/clang-format

# CI & Remote Validation (Mandatory CI-First)
gh workflow run ci.yml && gh run watch                         # Standard PR CI
gh workflow run release.yml --ref <branch> && gh run watch     # Remote multi-arch matrix dry run

# Proactive CI Quota Cancellation (cancel stale runs before push, or exclude current HEAD)
gh run list --branch $(git branch --show-current) --status in_progress
gh run cancel <run_id>
gh run list --branch $(git branch --show-current) --status in_progress --json databaseId,headSha --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" | xargs -r -n 1 gh run cancel
```

---

## 2. Operational Red Lines

- **No Direct Main Commits for Code**: Always develop via feature branches and Draft PRs. Pure docs and agent guidelines are exempt (`[skip ci]`).
- **Draft PR Pre-Creation**: Open a Draft PR with ALL checklist items UNCHECKED (`- [ ]`) before writing code.
- **No Multi-Task Lump Commits**: Every commit must map to exactly one `- [ ]` task in the PR checklist. Commit atomically, push, and check off `- [x]`.
- **Pre-Commit Quality Gate**: Run `mise run check:changed` or `hk run check --safe` before pushing.
- **Hardware-Adaptive Compilation**: If the GitHub user is `xifan2333`, **local compilation is strictly prohibited (never run `mise run build-debug`)**; rely exclusively on CI.
- **No Force-Push to Contributor Forks**: Never `git push -f` to external contributors' forks or branches.
- **PR Micro-Slicing**: Keep functional code changes < 300 lines; break large epics into stacked micro-PRs.
- **AI Attribution & Disclosure**: Append `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*` to PR descriptions, review fixes, and substantive comments.
- **Dialectical Review Bot Triage**: Never blindly follow CodeRabbit/Greptile suggestions. Report analysis to the user in chat before implementing fixes.
- **Non-Blocking Handover**: Never busy-wait on remote CI. After `gh pr ready`, output PR/run URLs and yield control.
- **Issue Lifecycle Complete Closure**: After squash-merging, switch to `main`, pull latest, and verify originating issue is `CLOSED`.
- **Proactive CI Quota Cancellation**: Cancel stale in-progress runs before pushing fixes or switching branches (`gh run cancel`). Never cancel runs on `main`.

---

## 3. Code Standards & Architecture Enforcement

> **Implementation vs. Review Separation**:
> Implementation agents focus on minimal, clean code. The review agent audits diffs against [`CODING_STANDARDS.md`](CODING_STANDARDS.md).

- **Code Standards**: Follow [`CODING_STANDARDS.md`](CODING_STANDARDS.md) for real-time audio safety, deep module architecture, SSOT common infrastructure, cognitive complexity ($\le 15$), and pristine 3rdparty governance.
- **Domain Modeling**: Follow [`GLOSSARY.md`](GLOSSARY.md) for all naming conventions and domain vocabulary.

---

## 4. Navigation Pointers (Progressive Disclosure)

| Need / Domain | Pointer | When to Reach |
| :--- | :--- | :--- |
| **Code Standards & Smells** | [`CODING_STANDARDS.md`](CODING_STANDARDS.md) | Writing C++20 code, reviewing PRs, checking linter rules |
| **Domain Glossary** | [`GLOSSARY.md`](GLOSSARY.md) | Naming domain primitives, structs, D-Bus methods, and tests |
| **Domain Docs Layout** | [`docs/agents/domain.md`](docs/agents/domain.md) | Exploring single-context vs multi-context documentation |
| **Issue Tracker Protocol** | [`docs/agents/issue-tracker.md`](docs/agents/issue-tracker.md) | Managing GitHub issues, sub-issues, and blocking edges |
| **Triage Labels** | [`docs/agents/triage-labels.md`](docs/agents/triage-labels.md) | Mapping canonical triage roles to issue labels |
| **Issue + Draft PR Workflow** | [issue-pr-workflow.md](.agents/skills/vinput-dev/references/issue-pr-workflow.md) | Setting up branches, PR checklists, and bot triage |
| **Compilation & Packaging** | [compilation-ci.md](.agents/skills/vinput-dev/references/compilation-ci.md) | Remote matrix validation, packaging channels, and SemVer rules |
| **Audio & Daemon Debugging** | [audio-debugging.md](.agents/skills/vinput-dev/references/audio-debugging.md) | Troubleshooting PipeWire capture, SPA buffers, and XRuns |
| **Dependency Governance** | [dependency-spec.md](.agents/skills/vinput-dev/references/dependency-spec.md) | Host packages vs. `3rdparty/` pristine vendoring |
| **Multi-Repo Ecosystem** | [ecosystem.md](.agents/skills/vinput-dev/references/ecosystem.md) | Registry, AUR packaging, and Flatpak automation |
| **C ABI Hard Boundaries** | [c-abi-boundaries.md](.agents/skills/vinput-dev/references/c-abi-boundaries.md) | Interfacing with PipeWire, sd-bus, POSIX, and libarchive |
| **Master Development Skill** | [SKILL.md](.agents/skills/vinput-dev/SKILL.md) | Development manual and code health checks |
