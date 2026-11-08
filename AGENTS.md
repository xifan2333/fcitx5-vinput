# fcitx5-vinput Agent Guide

Progressive operational guide, hard constraints, and routing index for AI coding agents working on `fcitx5-vinput`.

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

## 2. Hard Constraints (Red Lines)

1. **No Direct Main Commits for Code**: Never push implementation code, build configurations, or tests directly to `main`. Always develop via feature branches and Draft PRs.
   *Fast-track Exception (Doc & Agent Rules)*: Pure documentation, agent guides/skills (`AGENTS.md`, `CLAUDE.md`, `.agents/`), and non-functional textual edits that do not alter binary outputs or require CI validation are exempt from the Issue-PR lifecycle and may be committed directly to `main` with `[skip ci]`. Furthermore, `ci.yml` enforces an explicit C++ source and packaging whitelist (`paths`), guaranteeing zero CI runner consumption for pure documentation, agent rules, and workflow configuration changes.
2. **Draft PR Pre-Creation**: Open a Draft PR with ALL checklist items UNCHECKED (`- [ ]`) before writing any implementation code.
3. **No Multi-Task Lump Commits & Progressive Sync**: For changes developed via Draft PRs, every commit must map to exactly one `- [ ]` task in the PR checklist. After passing local quality gates (`mise run check:changed`), commit atomically, push to the branch, and update the PR checklist (`- [x]`) in lockstep for real-time traceability. Full matrix CI is automatically skipped during the Draft phase and triggers on `gh pr ready` (or manually on demand via `gh workflow run ci.yml --ref <branch>`).
4. **Pre-Commit Quality Gate**: Run `mise run check:changed` or `hk run check --safe` before pushing.
5. **Transparent Progress Tracking**: Always report progress against the PR checklist in both chat output and the remote PR description (e.g., "Completed 2 of 4 tasks; currently implementing task 3").
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
15. **Two-Tier Dependencies & Pristine 3rdparty (零篡改铁律)**: Never download at build time (`FetchContent` prohibited). Enforce Two-Tier Binary Boundary: Tier 1 system shared libraries (`*.so`: `fcitx5`, `pipewire`, `libsystemd`, `qt6`, `openssl`, `libarchive`, `gettext`) via host packages (`find_package REQUIRED` / `pkg_check_modules`); Tier 2 pure C++ header-only libraries (`fmt`, `spdlog`, `CLI11`, `httplib`, `glaze`, `tl-expected`, `mio`, `readerwriterqueue`, `subprocess`, `scope`, `indicators`, `base64`, `unicodelib`) self-contained under `3rdparty/` with CMake `SYSTEM INTERFACE` and registered in `THIRD_PARTY_NOTICES.md` to eliminate cross-distro header version skew. Hand-crafted infrastructure (Unicode/Base64 tables) is prohibited.
    - **Pristine 3rdparty (零篡改)**: Code under `3rdparty/` must remain untouched upstream snapshots. Never patch, reformat, or edit vendored code; all wrappers and adapters belong in `src/`.
16. **Proactive CI Quota Cancellation**: Guard Actions runner minutes. Before pushing follow-up fixes, amending commits, or switching tasks, proactively cancel stale in-progress runs (`gh run cancel`) to eliminate redundant compute. Never cancel runs on `main`.
17. **Dialectical Review Bot Triage & Pre-Action Reporting**: Never treat automated review bot outputs (CodeRabbit, Greptile, etc.) as unquestioned instructions. Agents must critically evaluate every finding against codebase architecture, C++20 standards, and false-positive likelihood. Before writing any fix code, the agent **MUST** report its analysis to the user in chat (what is accepted as a true issue, what is rejected as false positive and why, and what actions will be taken next), and only proceed after communicating the plan.
18. **Non-Blocking Handover (No CI Polling)**: Agents are strictly prohibited from busy-waiting or executing blocking polling loops (`gh run watch`, sleep loops, repeated `gh pr checks`). CI builds and review bots are asynchronous. Immediately after running `gh pr ready`, the agent must output the PR and Actions run URLs, summarize delivered changes, and yield control to the maintainer. Further actions (bot triage or merge) are event-driven upon user prompt.
19. **Issue-PR Lifecycle Complete Closure**: After squash-merging a PR, the agent **MUST** switch back to `main`, pull latest changes (`git checkout main && git pull`), and explicitly verify that the originating issue is in the `CLOSED` state (via `gh issue view <issue_id>`). If not automatically closed by GitHub keywords, manually close it (`gh issue close <issue_id>`). Never leave completed issues orphaned in an open state.
20. **SSOT Infrastructure Mandate (Zero Ad-hoc Utils)**: Never re-implement generic utility functions (e.g. ad-hoc string trimming, file reading, subprocess execution, ad-hoc JSON parsing). Always reuse project canonical infrastructure in `src/common/`:
    - JSON serialization & reflection: `glaze` (`#include <glaze/glaze.hpp>`).
    - Logging & tracing: Internal `spdlog` wrappers (`VLOG_INFO`, `VLOG_DEBUG`, `VLOG_ERROR`). Raw `fprintf(stderr)` and `std::cout` are forbidden.
    - String formatting & i18n: User-facing text must use `fmt::sprintf(_("..."), ...)`; internal string construction must use `fmt::format`.
    - Subprocess execution: `subprocess.hpp` and `src/common/utils/process_utils.h`.
    - Memory-mapped files: `mio`.
    - Real-time lock-free audio buffering: Single-producer single-consumer queue `readerwriterqueue`. Raw mutexes in the audio callback path are forbidden.
    - CLI subcommand parsing: `CLI11`.
21. **Duplication Triage & Human Decision Protocol**: Never commit copy-pasted procedural logic across files. Before finalizing PRs or code changes, agents must run `mise run check:dup` to inspect code clones. When clones are detected:
    - **Dialectical Triage**: The agent must critically classify findings into:
      - *Type A (AI Slop / Procedural Duplication)*: Redundant helpers or duplicated procedural logic across files $\to$ formulate a concrete plan to abstract into reusable modules under `src/common/`.
      - *Type B (Domain Boilerplate)*: Inherent protocol mappings, D-Bus XML dispatch stubs, or serialization tables $\to$ state concrete technical reasons for retaining.
    - **Mandatory Pre-Action Report to Human**: The agent **MUST** report the duplication summary (clone locations, lines, tokens) and its triage logic to the human maintainer in chat, and **await explicit human decision/confirmation** before writing refactoring code or finalizing the task.
22. **Strict Full-Project Linter Standards (Zero Compromise)**: Maintain full-file, zero-tolerance static analysis across every modified file (`lines-changed-only: false`, Clang-Tidy 21, Clang-Format, `-DVINPUT_ENABLE_WERROR=ON`). Never downgrade checks or compromise standards for legacy lines in touched files. Every touched file must strictly compile cleanly with zero warnings and zero diagnostic suppressions (`// NOLINT`).
23. **Architectural Boundary Mandate (Common Extraction vs. Domain Cohesion)**: Generic procedural parsing, string transformations, and low-level character scanners must never reside in domain modules (`src/cli/`, `src/daemon/`, `src/addon/`, `src/gui/`). All generic text and data primitives belong strictly in `src/common/` as stateless, reusable utilities. Business domains must strictly focus on high-level workflow orchestration and delegate stream processing to `src/common/`.
24. **Objective Cognitive Complexity & Space-Time Resource Gate**: Every function in touched files must satisfy `readability-function-cognitive-complexity.Threshold <= 15`. Nested loops and multi-level branching for stream parsing are strictly prohibited; standard library algorithms (`std::ranges::find_if`, `std::string_view::find_first_of`) must be used instead. Deliberately employ constant-factor space trade-offs (`reserve()`, immutable single-pass structs) to eliminate repeated heap reallocations and duplicate scans.
25. **Eight Pillars of Engineering Excellence & Hard/Soft Constraint Model**:
   - **🔴 Hard Constraints (Machine Blockers / 0-Tolerance)**:
     - *Zero-Warning & Zero-Suppression*: `-DVINPUT_ENABLE_WERROR=ON` across all targets. Never use `// NOLINT` or diagnostic pragmas.
     - *Formatting & Integrity*: Spotless automated formatting via `clang-format`, `cmake-format` (`cmakelang`), `shfmt`, and `typos` spell checking.
     - *Real-Time Audio RT Thread Safety*: PipeWire `processCallback` strictly forbids heap allocations (`new`, `malloc`, `make_unique`) and blocking locks (`std::mutex::lock`); lock-free SPSC (`readerwriterqueue`) is mandatory.
     - *Zero Stdout/Stderr Pollution*: `std::cout`, `std::cerr`, `printf`, and `puts` are strictly prohibited in `src/daemon/` and `src/addon/`; structured logging (`VLOG_*`) is mandatory.
     - *Memory & Lifetime Safety*: Zero dangling references, zero uninitialized variables, zero use-after-move (guaranteed by Clang-Tidy `bugprone-*` and CI ASan/UBSan).
   - **🟡 Soft Constraints (Design Heuristics / Contextual Flexibility)**:
     - *Guard Clauses & Control Flow*: Default to early returns and table-driven dispatch. Natural 2–3 level local branching is permitted; 4+ levels trigger `ast-grep` advisory warnings.
     - *Algorithmic Complexity*: Eliminate accidental $O(N^2)$ dynamic container iterations. Small constant loops, fixed 2D matrices, or coordinate passes are legitimate.
     - *Duplication Triage*: Governed by `jscpd` (Rust, `.jscpd.json`). Distinguish Type A (logic slop $\to$ abstract to `src/common/`) from Type B (inherent protocol/D-Bus boilerplate $\to$ retain with rationale).
     - *Idiomatic Modern C++20*: Prefer `std::string_view`, `std::span`, value semantics, and `expected<T, E>`. Pragmatic engineering is respected when interfacing with raw C ABIs.

---

## 3. Core Commands & Quality Gates

```bash
# Quality Gate (run before every commit/push - sub-second native execution)
mise run check:plan       # Preview execution plan and linters
mise run check:changed    # Validate only touched files (safe, fast, < 0.3s)
mise run check:ast        # Audit C++ AST structural anti-patterns via ast-grep
mise run check:dup        # Analyze code duplication across src/ (jscpd)
mise run check:typos      # Audit identifier and code spelling integrity (typos)
mise run check:cmake      # Validate CMake listfiles formatting (cmake-format)
mise run check:complexity # Audit cyclomatic & cognitive complexity (CCN <= 15, CogC <= 15)
mise run fix              # Auto-format all files via hk (clang-format, cmake-format, ruff, shfmt)

# CI & Remote Validation (CI-First strategy)
gh workflow run ci.yml && gh run watch                         # Standard PR CI
gh workflow run release.yml --ref <branch> && gh run watch     # Remote multi-arch matrix dry run

# Proactive CI Quota Cancellation (cancel stale runs before push, or exclude current HEAD)
gh run list --branch $(git branch --show-current) --status in_progress
gh run cancel <run_id>
gh run list --branch $(git branch --show-current) --status in_progress --json databaseId,headSha --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" | xargs -r -n 1 gh run cancel
```

---

## 4. Progressive Disclosure Index (Context Pointers)

To avoid context saturation, consult detailed reference manuals on demand when entering specific operational branches:

| Task Domain / Operational Need | Context Pointer & Target Path | When to Reach |
| :--- | :--- | :--- |
| **Issue + Draft PR Workflow** | [issue-pr-workflow.md](.agents/skills/vinput-dev/references/issue-pr-workflow.md) | Initializing branches, formatting PR checklists (`- [ ]`), single-item loops, review bot triage, and text-only fast-track rules. |
| **Compilation & Release CI** | [compilation-ci.md](.agents/skills/vinput-dev/references/compilation-ci.md) | Triggering remote matrix validation, multi-distro dry runs, SemVer rules, and `notification.json` breaking-change prompt specifications. |
| **Multi-Repo Ecosystem** | [ecosystem.md](.agents/skills/vinput-dev/references/ecosystem.md) | Adding cloud ASR providers / LLM scenes in `vinput-registry`, AUR updates in `aur-auto`, and Flatpak OSTree sync in `flatpak-auto`. |
| **Fork Contribution & Sync** | [fork-guide.md](.agents/skills/vinput-dev/references/fork-guide.md) | Working from an external fork, setting remotes, syncing with upstream, and avoiding fork force-pushes. |
| **Audio & Daemon Debugging** | [audio-debugging.md](.agents/skills/vinput-dev/references/audio-debugging.md) | Diagnosing PipeWire capture issues, SPA buffers, XRuns, `vinput-daemon.service` journalctl, and `VINPUT_DEBUG=1` tracing. |
| **C ABI Hard Boundaries** | [c-abi-boundaries.md](.agents/skills/vinput-dev/references/c-abi-boundaries.md) | Triaging review bot suggestions, identifying non-modernizable C boundaries (PipeWire, sd-bus, POSIX, libarchive), and enforcing C++20 standards elsewhere. |
| **Dependency Governance** | [dependency-spec.md](.agents/skills/vinput-dev/references/dependency-spec.md) | Adding/upgrading third-party libraries, `flake.nix` packages, `3rdparty/` vendoring, CMake `SYSTEM INTERFACE` isolation, and license auditing in `THIRD_PARTY_NOTICES.md`. |
| **Module Boundaries & Complexity** | [module-boundaries-and-complexity.md](.agents/skills/vinput-dev/references/module-boundaries-and-complexity.md) | Enforcing common vs. domain module boundaries, cognitive complexity limits (<=15), and space-time allocation trade-offs. |
| **Fcitx5 Addon Architecture** | Context7: `/fcitx/fcitx5` | Modifying `src/addon/`, Fcitx5 key event pipelines, addon lifecycle, and Fcitx5 official `fcitx::Configuration` / `fcitx::RawConfig`. |
| **Local Speech Engine (ASR)** | Context7: `/k2-fsa/sherpa-onnx` | Modifying `src/daemon/asr/`, sherpa-onnx offline/online recognizers, VAD, and model loading. |
| **Low-Latency Audio Capture** | Context7: `/pipewire/pipewire` | Modifying `src/daemon/audio/`, PipeWire C API, SPA pod formats, and stream lifecycle. |
| **CLI Parser & Subcommands** | Context7: `/cliutils/cli11` | Modifying `src/cli/`, CLI11 validator chains, subcommand hierarchy, and option bindings. |
| **GUI Framework (Qt 6)** | Context7: `/websites/doc_qt_io_qt-6` | Modifying `src/gui/`, Qt 6 Widgets, custom item delegates, layouts, and signal/slot wiring. |
| **Master Development Skill** | [SKILL.md](.agents/skills/vinput-dev/SKILL.md) | Full workflow index, Pre-PR code health checks, and cross-repository operational guide. |
