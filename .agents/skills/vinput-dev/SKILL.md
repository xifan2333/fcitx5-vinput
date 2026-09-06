---
name: vinput-dev
description: Complete development, fork contribution, code health check, Issue+PR workflow, and multi-repo ecosystem guide for fcitx5-vinput. Use when developing or refactoring daemon/addon C++20 code, contributing from a fork, syncing with upstream, running pre-PR clean-up inspections (代码体检), following the Issue+Draft PR lifecycle, working across related repositories (vinput-registry, aur-auto, flatpak-auto), debugging PipeWire audio or D-Bus services, or running CI and release pipelines. Trigger also on Chinese requests such as 开发 fcitx5-vinput、改代码、添加语音识别厂商/场景适配器、根据 issue 拆解开发、开 PR/提 PR、Fork 贡献与上游同步、代码体检与格式化自检、CI 编译/发版、调试 PipeWire 录音或排查 daemon 日志.
---

# fcitx5-vinput Unified Development Skill

This skill is the master operational manual for developing, contributing via forks, performing code health checks, and managing workflows across the `fcitx5-vinput` ecosystem.

---

## 1. Quick Repository Routing

Verify target repository before making changes:

| Target | Description | Local Path / Remote |
| :--- | :--- | :--- |
| **`fcitx5-vinput` (Core)** | C++20 Addon, Daemon, CLI, PipeWire Audio, Releases | `.` / [xifan2333/fcitx5-vinput](https://github.com/xifan2333/fcitx5-vinput) |
| **`vinput-registry`** | Cloud ASR provider scripts, LLM scene adapters, Model index | `~/Code/vinput-registry` / [xifan2333/vinput-registry](https://github.com/xifan2333/vinput-registry) |
| **`aur-auto`** | Arch Linux AUR packaging automation (`fcitx5-vinput-bin`) | `~/Code/aur-auto` / [xifan2333/aur-auto](https://github.com/xifan2333/aur-auto) |
| **`flatpak-auto`** | Flatpak OSTree repository & flatpakref automation | `~/Code/flatpak-auto` / [xifan2333/flatpak-auto](https://github.com/xifan2333/flatpak-auto) |

---

## 2. Core Architecture

- `src/addon/`: Fcitx5 addon (Qt/C++, hotkey triggers, floating UI, D-Bus client).
- `src/daemon/`: Background daemon (`vinput-daemon`, PipeWire audio capture, sherpa-onnx runtime, cloud ASR, LLM post-processing scenes, D-Bus service `org.fcitx.Vinput`).
- `src/cli/`: Standalone `vinput` CLI utility.
- `src/common/`: Shared structs, configuration loading (`nlohmann::json`), D-Bus interfaces.
- `po/` & `i18n/`: Localization catalogs.

---

## 3. Universal Code Quality Gate (`hk` / `mise`)

Always run verification before committing or reporting task completion:

```bash
# 1. Preview which steps match edited files
mise run check:plan

# 2. Check changed files safely
mise run check:changed

# 3. Structured JSONL for automated diagnostic loops (file, line, col)
hk run check --safe --format jsonl

# 4. Automatically fix formatting issues (clang-format, mise, whitespace)
mise run fix

# 5. Validate translations & configs
python3 scripts/check-i18n.py
```

---

## 4. Progressive Reference Guides (Read as Needed)

Follow progressive disclosure: consult specific reference files depending on your current task:

### Task: Implementing an Issue / Feature / Bugfix
Read **[references/issue-pr-workflow.md](references/issue-pr-workflow.md)**
- Dual-planning model: Task Planning (Issue breakdown) vs Quality Gate Pre-check (`hk --plan`).
- The 5-step Issue + Draft PR lifecycle (`gh pr create --draft`).
- Single-item local atomic commits, unified push on completion, and updating PR checklist checkboxes (`- [x]`).
- Finalizing, marking ready (`gh pr ready`), and squash merging.

### Task: Working in a Fork / Reviewing External PRs
Read **[references/fork-guide.md](references/fork-guide.md)**
- Setting up `upstream` remote and rebasing on `upstream/main`.
- The 4-step Pre-PR Code Health Check pipeline and diff audit checklist.
- Conventional commit rules and upstream PR submission (`gh pr create --repo xifan2333/fcitx5-vinput`).
- Maintainer collaboration etiquette: Never `git push -f` to external contributors' forks or branches. If a PR has conflicts from an earlier merge, ask the author to rebase or handle integration on upstream branches only.

### Task: Compiling, Building, or Running CI
Read **[references/compilation-ci.md](references/compilation-ci.md)**
- Hardware-adaptive compilation: CI-first on modest machines; local incremental builds on powerful machines.
- The two usages of `release.yml`: Remote multi-arch matrix validation (Dry Run) vs official release publishing.
- GitHub Actions workflow matrix (`ci.yml`, `channels.yml`, `nix-cache.yml`).
- Release notification specification: breaking change "Prompt for Agent" format and 5-version retention policy.

### Task: Modifying Cloud ASR, LLM Scenes, AUR, or Flatpak Repos
Read **[references/ecosystem.md](references/ecosystem.md)**
- Adding cloud ASR provider scripts (`entry.py`, `VINPUT_ASR_*` env vars, stdlib only).
- Adding LLM scene adapters and updating `i18n/*.json`.
- Tracking upstream releases and PKGBUILD updates in `aur-auto`.
- Managing Flatpak bundle imports and OSTree repo syncing in `flatpak-auto`.

### Task: Debugging Audio Capture or Daemon Issues
Read **[references/audio-debugging.md](references/audio-debugging.md)**
- Inspecting live `vinput-daemon.service` journal logs and `VINPUT_DEBUG=1` mode.
- Benchmarking PipeWire stream cold-start latency.
