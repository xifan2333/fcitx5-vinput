---
name: vinput-dev
description: Complete development, fork contribution, code health check, Issue+PR workflow, dependency governance, and multi-repo ecosystem guide for fcitx5-vinput. Use when developing or refactoring daemon/addon C++20 code, introducing or managing third-party dependencies (CMake/flake.nix/3rdparty), contributing from a fork, syncing with upstream, running pre-PR clean-up inspections (代码体检), following the Issue+Draft PR lifecycle, working across related repositories (vinput-registry, aur-auto, flatpak-auto), debugging PipeWire audio or D-Bus services, or running CI and release pipelines. Trigger also on Chinese requests such as 开发 fcitx5-vinput、改代码、添加语音识别厂商/场景适配器、根据 issue 拆解开发、开 PR/提 PR、Fork 贡献与上游同步、代码体检与格式化自检、CI 编译/发版、调试 PipeWire 录音或排查 daemon 日志、引入/治理第三方依赖、配置 CMake/Nix 依赖、3rdparty 库内嵌.
---

# fcitx5-vinput Unified Development Skill

Master router and operational manual for developing, contributing via forks, performing code health checks, and managing workflows across the `fcitx5-vinput` ecosystem.

---

## 1. Governance & Lifecycle Skills

The development workflow in `fcitx5-vinput` separates implementation, architecture, review, and diagnostic phases into specialized, composable skills:

```
[Phase -1: Design]       vinput-architect  -> Settle deep modules, C++20 types, audio RT safety
[Phase  0: Worktree]     git worktree      -> Isolate development from active main branches
[Phase  1: Implement]    vinput-dev        -> Draft PR, atomic commits, progressive checklist sync
[Phase  2: Pre-PR Check] vinput-review     -> Two-Axis review (Standards Axis + Spec Axis)
[Phase  3: Self-Evolve]  vinput-correct    -> Convert review feedback into structural ast-grep/checks
[Diagnosis: On Demand]   vinput-diagnose   -> PipeWire, SPA ringbuffer, XRuns, D-Bus triage
```

- **[`vinput-architect`](../vinput-architect/SKILL.md)**: Deep module boundaries, deletion test, RAII ownership, and zero-allocation audio callbacks.
- **[`vinput-review`](../vinput-review/SKILL.md)**: Two-axis review along `CODING_STANDARDS.md` (Standards Axis) and Issue/Spec (Spec Axis) in parallel.
- **[`vinput-correct`](../vinput-correct/SKILL.md)**: Continuous governance engine converting review corrections into AST/linter checks (5-level defense hierarchy).
- **[`vinput-diagnose`](../vinput-diagnose/SKILL.md)**: PipeWire capture debugging, buffer dropouts (XRuns), D-Bus monitoring, and runtime tracing.

---

## 2. Universal Quality Gates

Run local validation before any commit or reporting task completion:

```bash
# Preview matched rules and tools
mise run check:plan

# Fast validation on modified files (mandatory before git commit)
mise run check:changed

# Analyze code duplication across src/
mise run check:dup

# Audit C++ AST structural anti-patterns
mise run check:ast

# Audit cyclomatic & cognitive complexity (CCN <= 15, CogC <= 15)
mise run check:complexity

# Verify config schema migrations
mise run check:migration

# Auto-format C++ / JSON / Python / Markdown
mise run fix
```

---

## 3. Ecosystem Repository Routing

Verify target repository boundaries before making any changes:

| Target Component | Repository & Path | Primary Role |
| :--- | :--- | :--- |
| **Core Engine** | `.` / [xifan2333/fcitx5-vinput](https://github.com/xifan2333/fcitx5-vinput) | C++20 Fcitx5 addon, background daemon, CLI, PipeWire capture, sherpa-onnx runtime. |
| **Catalog Registry** | `~/Code/vinput-registry` / [xifan2333/vinput-registry](https://github.com/xifan2333/vinput-registry) | Cloud ASR provider scripts, LLM scene adapters, and offline model catalogs. |
| **Arch Packaging** | `~/Code/aur-auto` / [xifan2333/aur-auto](https://github.com/xifan2333/aur-auto) | Arch User Repository (`fcitx5-vinput-bin`) release tracking and clean chroot builds. |
| **Flatpak Automation** | `~/Code/flatpak-auto` / [xifan2333/flatpak-auto](https://github.com/xifan2333/flatpak-auto) | Flatpak bundle builds, OSTree repo importing, and `.flatpakref` generation. |

---

## 4. Task-Driven Reference Manuals

| Operational Need | Manual | Key Takeaways |
| :--- | :--- | :--- |
| **Issue + Draft PR Workflow** | [issue-pr-workflow.md](references/issue-pr-workflow.md) | Branch setup, PR checklists (`- [ ]`), single-item atomic commits, bot triage |
| **Compilation & Release CI** | [compilation-ci.md](references/compilation-ci.md) | Maintainer no-local-build rule, multi-arch matrix dry runs, SemVer minor bumps |
| **Fork Contribution & Sync** | [references/fork-guide.md](references/fork-guide.md) | Clean chroot builds, remote syncing, zero force-push to personal contributor forks |
| **Multi-Repo Ecosystem** | [references/ecosystem.md](references/ecosystem.md) | Registry scripts (zero pip deps), AUR updates, Flatpak OSTree sync |
| **Dependency Governance** | [references/dependency-spec.md](references/dependency-spec.md) | Tier 1 host packages vs. Tier 2 `3rdparty/` pristine vendoring (zero edits) |
| **Audio & PipeWire Tracing** | [references/audio-debugging.md](references/audio-debugging.md) | Troubleshooting SPA buffer dropouts, XRuns, `VINPUT_DEBUG=1` |
| **C ABI Hard Boundaries** | [references/c-abi-boundaries.md](references/c-abi-boundaries.md) | PipeWire/sd-bus/libarchive C boundaries vs. modern C++20 business logic |
| **Module Depth & Complexity** | [references/module-boundaries-and-complexity.md](references/module-boundaries-and-complexity.md) | Deep modules over shallow wrappers; CogC $\le 15$ ceiling |

---

## 5. Upstream Pillar Grounding (Context7 Pointers)

Before implementing non-trivial logic across the five core pillars, query the authoritative Context7 library to prevent architectural hallucinations:

- **Fcitx5 Addon (`src/addon/`)**: Context7: `/fcitx/fcitx5`.
- **Local Speech Runtime (`src/daemon/asr/`)**: Context7: `/k2-fsa/sherpa-onnx`.
- **PipeWire Capture (`src/daemon/audio/`)**: Context7: `/pipewire/pipewire`.
- **CLI Subcommand Tree (`src/cli/`)**: Context7: `/cliutils/cli11`.
- **Desktop GUI (`src/gui/`)**: Context7: `/websites/doc_qt_io_qt-6`.
