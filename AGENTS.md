# fcitx5-vinput Agent Guide

Progressive operational guide, hard constraints, and routing index for AI coding agents working on `fcitx5-vinput`.

---

## 1. Project Overview & Architecture

`fcitx5-vinput` is an offline/cloud voice input system for Fcitx5 providing local (sherpa-onnx) and cloud ASR, LLM post-processing, and cross-distro packaging.

- `src/addon/`: Fcitx5 input method addon (Qt/C++, hotkey triggers like `Alt_R` / `Shift_R`, push-to-talk, D-Bus client).
- `src/daemon/`: Core daemon (`vinput-daemon`, PipeWire audio capture, sherpa-onnx inference, cloud ASR engines, LLM scene transformations, D-Bus service `org.fcitx.Vinput`).
- `src/cli/`: Standalone `vinput` CLI for manual recording, profile switching, and status inspection.
- `src/common/`: Shared types, configuration structs (`nlohmann-json`), isolated ConfigMigration engine (`config/config_migration.cpp`), D-Bus XML interfaces.
- `src/gui/`: Standalone Qt 6 configuration GUI (`vinput-gui`).
- `po/` & `i18n/`: Translations (Gettext and Qt `.ts` / `.qm`).

---

## 2. Hard Constraints (Red Lines)

1. **No Direct Main Commits**: Never push implementation code directly to `main`. Always develop via feature branches and Draft PRs.
2. **Draft PR Pre-Creation**: Open a Draft PR with ALL checklist items UNCHECKED (`- [ ]`) before writing any implementation code.
3. **No Multi-Task Lump Commits**: Every commit must map to exactly one `- [ ]` task in the PR checklist. Keep commits local until all checklist tasks pass local quality gates.
4. **Pre-Commit Quality Gate**: Run `mise run check:changed` or `hk run check --safe` before pushing.
5. **Transparent Progress Tracking**: Always report progress against the PR checklist (e.g., "Completed 2 of 4 tasks; currently implementing task 3").
6. **Hardware-Adaptive & User-Specific Compilation**:
   - If the GitHub user is `xifan2333` (maintainer environment), **local compilation is strictly prohibited (never run `mise run build-debug` or any heavy local build commands)**; the CI pipeline must be prioritized (rely exclusively on `ci.yml` / `release.yml`).
   - For other users on modest hardware, also prioritize GitHub Actions CI over heavy local builds.
7. **User-Facing Strings**: Wrap all user-visible strings in `_("...")` or `ki18n` for gettext localization. Run `mise run check-i18n` to validate.
8. **No Force-Pushing to Contributor Forks**: Never `git push -f` to external contributors' personal forks or PR branches, even if GitHub's "Allow edits by maintainers" is technically enabled. Overwriting a contributor's commit history breaks their local workspace and violates open-source collaboration boundaries. Handle conflicts within upstream branches or request rebasing politely.
9. **Breaking Config Changes Require ConfigMigration**: If modifying user-facing keys in `config.json` or `vinput.conf`, add a versioned step in `src/common/config/config_migration.cpp` (`RegisteredSteps`) in the **same PR** (using `RenameField` / `EnsureField` / `ReplaceIniKey` / `RemoveIniKey`). Never add runtime compatibility aliases. Users and agents migrate with `vinput config migrate`.
10. **Zero-Tolerance on Suppressing Diagnostics**: Never use `// NOLINT`, `#pragma GCC/clang diagnostic ignored`, or `-Wno-*` compiler flags in CMakeLists.txt to silence static analysis or compiler warnings. Refactor types, add missing standard headers, or restructure code so clean compilation and `clang-tidy` passes without exclusions.
11. **PR Micro-Slicing & Stacked PRs**: Keep functional code changes <300 lines (excluding tests/generated docs); decompose complex epics into stacked micro-PRs (e.g. config -> daemon -> GUI). Do not bundle unrelated refactoring, CI fixes, and multi-subsystem features into a monolithic PR.
12. **AI Attribution & Disclosure**: Append `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*` to PR descriptions, review fixes, and substantive review comments.
13. **Upstream-First API Policy**: Prohibited from writing custom glue or ad-hoc state machines across the five pillars. Ground implementations via Context7 before making non-trivial modifications.
14. **Semantic Versioning Integrity**: Schema changes require a MINOR bump (`x.Y.0`). Never release breaking configuration changes as a PATCH (`x.y.Z`).

---

## 3. Core Commands & Quality Gates

```bash
# Quality Gate (run before every commit/push)
mise run check:plan       # Preview execution plan and linters
mise run check:changed    # Validate only touched files (safe, fast)
mise run fix              # Auto-format all files via hk/clang-format

# CI & Remote Validation (CI-First strategy)
gh workflow run ci.yml && gh run watch                         # Standard PR CI
gh workflow run release.yml --ref <branch> && gh run watch     # Remote multi-arch matrix dry run
```

---

## 4. Progressive Disclosure Index (Context Pointers)

To avoid context saturation, consult detailed reference manuals on demand when entering specific operational branches:

| Task Domain / Operational Need | Context Pointer & Target Path | When to Reach |
| :--- | :--- | :--- |
| **Issue + Draft PR Workflow** | [issue-pr-workflow.md](.agents/skills/vinput-dev/references/issue-pr-workflow.md) | Initializing branches, formatting PR checklists (`- [ ]`), executing single-item loops, handling CodeRabbit/Greptile review bot triage. |
| **Compilation & Release CI** | [compilation-ci.md](.agents/skills/vinput-dev/references/compilation-ci.md) | Triggering remote matrix validation, multi-distro dry runs, SemVer rules, and `notification.json` breaking-change prompt specifications. |
| **Multi-Repo Ecosystem** | [ecosystem.md](.agents/skills/vinput-dev/references/ecosystem.md) | Adding cloud ASR providers / LLM scenes in `vinput-registry`, AUR updates in `aur-auto`, and Flatpak OSTree sync in `flatpak-auto`. |
| **Fork Contribution & Sync** | [fork-guide.md](.agents/skills/vinput-dev/references/fork-guide.md) | Working from an external fork, setting remotes, syncing with upstream, and avoiding fork force-pushes. |
| **Audio & Daemon Debugging** | [audio-debugging.md](.agents/skills/vinput-dev/references/audio-debugging.md) | Diagnosing PipeWire capture issues, SPA buffers, XRuns, `vinput-daemon.service` journalctl, and `VINPUT_DEBUG=1` tracing. |
| **Fcitx5 Addon Architecture** | Context7: `/fcitx/fcitx5` | Modifying `src/addon/`, Fcitx5 key event pipelines, addon lifecycle, and Fcitx5 official `fcitx::Configuration` / `fcitx::RawConfig`. |
| **Local Speech Engine (ASR)** | Context7: `/k2-fsa/sherpa-onnx` | Modifying `src/daemon/asr/`, sherpa-onnx offline/online recognizers, VAD, and model loading. |
| **Low-Latency Audio Capture** | Context7: `/pipewire/pipewire` | Modifying `src/daemon/audio/`, PipeWire C API, SPA pod formats, and stream lifecycle. |
| **CLI Parser & Subcommands** | Context7: `/cliutils/cli11` | Modifying `src/cli/`, CLI11 validator chains, subcommand hierarchy, and option bindings. |
| **GUI Framework (Qt 6)** | Context7: `/websites/doc_qt_io_qt-6` | Modifying `src/gui/`, Qt 6 Widgets, custom item delegates, layouts, and signal/slot wiring. |
| **Master Development Skill** | [SKILL.md](.agents/skills/vinput-dev/SKILL.md) | Full workflow index, Pre-PR code health checks, and cross-repository operational guide. |
