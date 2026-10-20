# Claude Code Guide

Compact index and operational quick-reference for Claude Code on `fcitx5-vinput`. For complete project architecture and detailed operational standards, see [AGENTS.md](AGENTS.md).

---

## 1. Core Commands & Quality Gates

```bash
# Local Quality Gate (Run before every commit and push)
mise run check:plan       # Preview execution plan and linters
mise run check:changed    # Validate only touched files (safe, fast)
mise run fix              # Auto-format all modified files via hk/clang-format

# CI & Remote Validation (Mandatory CI-First)
gh workflow run ci.yml && gh run watch                         # Standard PR CI
gh workflow run release.yml --ref <branch> && gh run watch     # Remote multi-arch matrix dry run
```

---

## 2. Hard Constraints (Red Lines)

- **No Direct Main Commits for Code**: Always develop via feature branches and Draft PRs. Pure documentation, text edits, and agent guidelines/skills (`AGENTS.md`, `CLAUDE.md`, `.agents/`) are exempt and may be committed directly to `main` using `[skip ci]`. CI workflows enforce an explicit source whitelist (`paths`), guaranteeing zero runner consumption for docs, rules, and workflow edits.
- **Draft PR Pre-Creation**: Open a Draft PR with ALL checklist items UNCHECKED (`- [ ]`) before writing code.
- **No Multi-Task Lump Commits & Progressive Sync**: For changes developed via Draft PRs, every commit must map to exactly one `- [ ]` task in the PR checklist. Commit atomically, push to the branch, and check off `- [x]` in lockstep. Full matrix CI is automatically skipped in Draft and triggers on `gh pr ready` (or manual `gh workflow run ci.yml --ref <branch>`).
- **Pre-Commit Quality Gate**: Run `mise run check:changed` or `hk run check --safe` before pushing.
- **Hardware-Adaptive Compilation**: If the GitHub user is `xifan2333`, **local compilation is strictly prohibited (never run `mise run build-debug`)**; the CI pipeline must be prioritized (`ci.yml` / `release.yml`).
- **No Force-Push to Contributor Forks**: Never `git push -f` to external contributors' forks or branches.
- **Breaking Config Changes**: If user-facing keys in `config.json` / `vinput.conf` change incompatibly, add a versioned ConfigMigration step (`src/common/config/config_migration.cpp` `RegisteredSteps`) in the **same PR**. No runtime aliases.
- **Zero-Tolerance on Suppressing Diagnostics**: Never add `// NOLINT`, `#pragma GCC/clang diagnostic ignored`, or `-Wno-*` in CMake. Fix the root cause cleanly.
- **Three-Tier Priority-First Dependency Governance**: No network downloads or `FetchContent` during build. Follow Three-Tier Priority Cascade: Tier 1/2 universal libraries via host packages (`find_package REQUIRED`); Tier 3 missing/single-header libraries vendored under `3rdparty/` with CMake `SYSTEM INTERFACE` and registered in `THIRD_PARTY_NOTICES.md`. Hand-crafted infrastructure (Unicode/Base64 tables) is forbidden.
- **PR Micro-Slicing**: Keep functional code changes <300 lines; break large epics into stacked micro-PRs.
- **AI Attribution & Disclosure**: Append `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*` to PR descriptions, review fixes, and substantive comments.
- **Dialectical Review Bot Triage & Pre-Action Reporting**: Never blindly follow CodeRabbit/Greptile suggestions. Critically evaluate findings against C++20 and project architecture. Before writing fix code, the agent MUST explicitly report its analysis (true issues vs. false positives and next actions) to the user in chat.
- **Non-Blocking Handover (No CI Polling)**: Never busy-wait on remote CI (`gh run watch` or sleep polling). After `gh pr ready`, output PR/run URLs and yield control. Further review triage or merge actions are event-driven upon user prompt.
- **Upstream-First API Policy (Context7)**:
  - `src/addon/`: Fcitx5 (`fcitx/fcitx5`)
  - `src/daemon/asr/`: sherpa-onnx (`k2-fsa/sherpa-onnx`)
  - `src/daemon/audio/`: PipeWire (`pipewire/pipewire`)
  - `src/cli/`: CLI11 (`cliutils/cli11`)
  - `src/gui/`: Qt 6 (`websites/doc_qt_io_qt-6`)
  - Query Context7 before modifying these pillars to maintain upstream compatibility.

---

## 3. Progressive Disclosure Index (Context Pointers)

Consult detailed manuals on demand when entering specific operational workflows:

| Task Branch / Need | Context Pointer & Target Path | When to Reach |
| :--- | :--- | :--- |
| **Issue + Draft PR Workflow** | [.agents/skills/vinput-dev/references/issue-pr-workflow.md](.agents/skills/vinput-dev/references/issue-pr-workflow.md) | Initializing branches, formatting PR checklists (`- [ ]`), single-item execution loops, handling review bots, and direct-to-main fast-track. |
| **Compilation & Release CI** | [.agents/skills/vinput-dev/references/compilation-ci.md](.agents/skills/vinput-dev/references/compilation-ci.md) | Remote matrix validation, packaging dry runs, SemVer rules, and `notification.json` specifications. |
| **Multi-Repo Ecosystem** | [.agents/skills/vinput-dev/references/ecosystem.md](.agents/skills/vinput-dev/references/ecosystem.md) | Adding cloud ASR / LLM scenes in `vinput-registry`, AUR updates in `aur-auto`, Flatpak builds in `flatpak-auto`. |
| **Fork Contribution & Sync** | [.agents/skills/vinput-dev/references/fork-guide.md](.agents/skills/vinput-dev/references/fork-guide.md) | Working from an external fork, setting remotes, syncing with upstream. |
| **Audio & Daemon Debugging** | [.agents/skills/vinput-dev/references/audio-debugging.md](.agents/skills/vinput-dev/references/audio-debugging.md) | Troubleshooting PipeWire audio capture, XRuns, D-Bus service crashes, reading systemd logs. |
| **Dependency Governance** | [.agents/skills/vinput-dev/references/dependency-spec.md](.agents/skills/vinput-dev/references/dependency-spec.md) | Adding/upgrading 3rd-party libraries, `flake.nix` packages, `3rdparty/` vendoring, CMake `SYSTEM INTERFACE` isolation, and license notices. |
| **Master Development Skill** | [.agents/skills/vinput-dev/SKILL.md](.agents/skills/vinput-dev/SKILL.md) | Master development manual, Pre-PR code health checks, and cross-repo guides. |
