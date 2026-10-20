---
name: vinput-dev
description: Complete development, fork contribution, code health check, Issue+PR workflow, dependency governance, and multi-repo ecosystem guide for fcitx5-vinput. Use when developing or refactoring daemon/addon C++20 code, introducing or managing third-party dependencies (CMake/flake.nix/3rdparty), contributing from a fork, syncing with upstream, running pre-PR clean-up inspections (代码体检), following the Issue+Draft PR lifecycle, working across related repositories (vinput-registry, aur-auto, flatpak-auto), debugging PipeWire audio or D-Bus services, or running CI and release pipelines. Trigger also on Chinese requests such as 开发 fcitx5-vinput、改代码、添加语音识别厂商/场景适配器、根据 issue 拆解开发、开 PR/提 PR、Fork 贡献与上游同步、代码体检与格式化自检、CI 编译/发版、调试 PipeWire 录音或排查 daemon 日志、引入/治理第三方依赖、配置 CMake/Nix 依赖、3rdparty 库内嵌.
---

# fcitx5-vinput Unified Development Skill

Master router and operational manual for developing, contributing via forks, performing code health checks, and managing workflows across the `fcitx5-vinput` ecosystem.

---

## 1. Ecosystem Repository Routing

Verify target repository boundaries before making any changes:

| Target Component | Repository & Path | Primary Role |
| :--- | :--- | :--- |
| **Core Engine** | `.` / [xifan2333/fcitx5-vinput](https://github.com/xifan2333/fcitx5-vinput) | C++20 Fcitx5 addon, background daemon, CLI, PipeWire capture, sherpa-onnx runtime. |
| **Catalog Registry** | `~/Code/vinput-registry` / [xifan2333/vinput-registry](https://github.com/xifan2333/vinput-registry) | Cloud ASR provider scripts, LLM scene adapters, and offline model catalogs. |
| **Arch Packaging** | `~/Code/aur-auto` / [xifan2333/aur-auto](https://github.com/xifan2333/aur-auto) | Arch User Repository (`fcitx5-vinput-bin`) release tracking and clean chroot builds. |
| **Flatpak Automation** | `~/Code/flatpak-auto` / [xifan2333/flatpak-auto](https://github.com/xifan2333/flatpak-auto) | Flatpak bundle builds, OSTree repo importing, and `.flatpakref` generation. |

---

## 2. Universal Pre-Commit Quality Gate

Run local validation before any commit or reporting task completion:

```bash
# Preview matched rules and tools
mise run check:plan

# Fast validation on modified files (mandatory before git commit)
mise run check:changed

# Automated JSONL diagnostics stream
hk run check --safe --format jsonl

# Auto-format C++ / JSON / Markdown
mise run fix

# Validate Gettext translations & JSON schema
python3 scripts/check-i18n.py
```

---

## 3. Task-Driven Progressive Pointers

Agents must progressively load detailed operational manuals according to the current task branch:

### Branch: Developing Features or Bugfixes
Read **[references/issue-pr-workflow.md](references/issue-pr-workflow.md)**
- **Trigger**: When starting any new feature, bugfix, or functional refactoring in `fcitx5-vinput` (pure documentation and agent skill changes follow the direct-to-main fast-track).
- **Core Standard**: Strict Issue $\to$ Pre-code Draft PR (all `- [ ]` unchecked) $\to$ single-item local atomic commit $\to$ unified push on full checklist completion $\to$ review-fix triage loop.
- **Micro-slicing**: Keep functional code diffs under 300 lines; break large epics into stacked micro-PRs.
- **Config Migration**: Breaking config changes require updating `src/common/config/config_migration.cpp` in the same PR.
- **Fast-Track Policy**: Non-functional text, documentation, and agent rules bypass the Issue/PR lifecycle and may be committed directly to `main` with `[skip ci]`.

### Branch: Compiling, Testing, or Triggering CI
Read **[references/compilation-ci.md](references/compilation-ci.md)**
- **Trigger**: When building, testing, verifying packaging matrices, or preparing releases.
- **Core Standard**: If GitHub user is `xifan2333`, **local compilation is strictly prohibited (never run `mise run build-debug`)**; rely exclusively on `gh workflow run ci.yml` and `release.yml`. Config schema changes strictly require a MINOR version bump and ConfigMigration registration.

### Branch: Working from an External Fork
Read **[references/fork-guide.md](references/fork-guide.md)**
- **Trigger**: When contributing from a personal fork or reviewing community contributions.
- **Core Standard**: Follow the 4-step Pre-PR Code Health Check. Never force-push (`git push -f`) to external contributors' forks or branches.

### Branch: Modifying Cloud ASR Providers, LLM Scenes, or Packaging
Read **[references/ecosystem.md](references/ecosystem.md)**
- **Trigger**: When adding cloud speech scripts, scene prompts, AUR PKGBUILD updates, or Flatpak manifests.
- **Core Standard**: Cloud ASR and LLM scripts in `vinput-registry` must use Python standard library only (zero pip dependencies) and standard `VINPUT_ASR_*` environment variables.

### Branch: Managing Dependencies & Third-Party Modernization
Read **[references/dependency-spec.md](references/dependency-spec.md)**
- **Trigger**: When adding or upgrading third-party C++ libraries, configuring CMake dependencies, updating `flake.nix` `buildInputs`, vendoring header-only libraries under `3rdparty/`, or touching `THIRD_PARTY_NOTICES.md`.
- **Core Standard**: Strict Three-Tier Priority Cascade (System-First $\to$ 3rdparty Vendored Fallback). CMake must never download over network (`FetchContent` git clone prohibited for offline sandbox compatibility). Universal libraries use `find_package REQUIRED`. Vendored libraries in `3rdparty/` must be isolated via CMake `SYSTEM INTERFACE` to prevent `clang-tidy` diagnostic pollution without suppressing warnings. Hand-crafted infrastructure (e.g. ad-hoc Unicode/Base64 tables) is forbidden.

### Branch: Diagnosing Audio Streams or Daemon Runtime
Read **[references/audio-debugging.md](references/audio-debugging.md)**
- **Trigger**: When audio capture fails, PipeWire reports XRuns, D-Bus service crashes, or analyzing daemon logs.
- **Core Standard**: Inspect systemd user logs via `journalctl --user-unit vinput-daemon -f` and run with `VINPUT_LOG_LEVEL=debug`.

---

## 4. Upstream Pillar Grounding (Context7 Pointers)

Before implementing non-trivial logic across the five core pillars, query the authoritative Context7 library to prevent architectural hallucinations:

- **Fcitx5 Addon (`src/addon/`)**: Fetch canonical API patterns via Context7: `/fcitx/fcitx5`.
- **Local Speech Runtime (`src/daemon/asr/`)**: Fetch sherpa-onnx recognizer/VAD patterns via Context7: `/k2-fsa/sherpa-onnx`.
- **PipeWire Capture (`src/daemon/audio/`)**: Fetch low-latency audio capture & SPA formats via Context7: `/pipewire/pipewire`.
- **CLI Subcommand Tree (`src/cli/`)**: Fetch validator & subcommand architecture via Context7: `/cliutils/cli11`.
- **Desktop GUI (`src/gui/`)**: Fetch Qt 6 widgets, delegates, and layouts via Context7: `/websites/doc_qt_io_qt-6`.
