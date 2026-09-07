# Claude Code Guide

See [AGENTS.md](AGENTS.md) for full architecture, dual-planning model, compilation strategies, Issue+PR workflow, and hard constraints.

## Quick CLI Reference

- **Dual-Planning Model**:
  - *Phase A (Task Planning - BEFORE CODING)*:
    `gh issue view <id>` -> `git checkout -b <type>/issue-<id>-<desc>` -> `git commit --allow-empty -m "chore: init draft pr"` -> `git push -u origin <branch>` -> `gh pr create --draft --body "Closes #<id>\n\n### Tasks\n- [ ] 1. ..."`
  - *Phase B (Quality Gate Pre-check - AFTER EDIT)*: `mise run check:plan`
- **Execution Loop (ONE ITEM AT A TIME)**:
  - Code task N -> `mise run check:changed` (or `hk fix`) -> local atomic commit -> Repeat for all tasks
  - Structured fix stream: `hk run check --safe --format jsonl`
  - Finish: `git push origin <branch>` -> `gh pr edit --body` (update tasks to `- [x]`) -> `gh pr ready`
- **Review-Fix Loop (POST-READY)**:
  - Check CI & Bots: `gh pr checks` and `gh pr view --comments`
  - Ingest feedback: Extract `> Prompt for AI Agents` from CodeRabbit, or `Proposed fix` from Cursor Bugbot
  - Defensive fix & local verify: `mise run check:changed` -> `git commit -m "fix(review): ..."` -> `git push`
  - Finalize: Once all CI and bot checks are green, `gh pr merge --squash --delete-branch`
- **Quality Gate Tasks (`mise`)**:
  - `mise run check:changed`: Run safe check only on modified files
  - `mise run check:plan`: Preview execution plan without running tools
  - `mise run check:safe`: Run safe check across the repo
  - `mise run fix`: Auto-format all files
  - `mise run check`: Full repository validation
- **Compilation (Hardware-Adaptive)**:
  - *Modest Hardware (CI-First)*: `gh workflow run ci.yml && gh run watch`
  - *Pre-release Matrix Dry Build*: `gh workflow run release.yml && gh run watch`
  - *Local Incremental (Powerful hardware only)*: `mise run dev` -> `mise run build-debug`

## Hard Constraints (Red Lines)
- **No Direct Main Commits**: Always develop via feature branches and Draft PRs.
- **No Force-Push to Contributor Forks**: Never `git push -f` to external contributors' forks or branches. Ask them to rebase or handle conflicts within upstream branches only.
- **Pre-Commit Quality Gate**: Run `mise run check:changed` or `hk run check --safe` before pushing.
- **Hardware-Adaptive Compilation**: Prioritize GitHub Actions CI over heavy local builds on modest hardware.
- **Breaking Config Changes**: If user-facing keys in `config.json` / `vinput.conf` change incompatibly, add a versioned ConfigMigration step (`src/common/config/config_migration.cpp` `RegisteredSteps`) in the **same PR**. No runtime aliases. Users run `vinput config migrate`.
- **Zero-Tolerance on Suppressing Diagnostics**: Never add `// NOLINT`, `#pragma GCC/clang diagnostic ignored`, or `-Wno-*` in CMake. Fix the root cause cleanly.
- **PR Micro-Slicing**: Keep functional code changes <300 lines; break large epics into stacked micro-PRs.
- **AI Attribution & Disclosure**: Append `*AI-assisted — Tool: <tool>; model: <model>; version: <version>.*` to PR descriptions and significant automated review fixes.
- **Notification Specification (`notification.json`)**:
  - *Breaking Changes*: "Prompt for Agent" with target version, `vinput init -f`, and `vinput config migrate [--dry-run]`. Do not list per-key recipes. Retain across 5 patch releases (or until next minor).
  - *Standard Releases*: Concise bilingual feature/fix changelog.

## Ecosystem Repositories & Upstream
- **Core Engine (C++20)**: [xifan2333/fcitx5-vinput](https://github.com/xifan2333/fcitx5-vinput) (`~/Code/fcitx5-vinput`)
- **Cloud ASR / LLM Scenes Registry**: [xifan2333/vinput-registry](https://github.com/xifan2333/vinput-registry) (`~/Code/vinput-registry`)
- **Arch AUR Packaging Automation**: [xifan2333/aur-auto](https://github.com/xifan2333/aur-auto) (`~/Code/aur-auto`)
- **Flatpak OSTree Repository Automation**: [xifan2333/flatpak-auto](https://github.com/xifan2333/flatpak-auto) (`~/Code/flatpak-auto`)
- **Upstream Input Method Framework**: [fcitx/fcitx5](https://github.com/fcitx/fcitx5) (API reference, key/modifier event loop)
- **Upstream Local ASR Inference**: [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (VAD & onnx model runtime)

## Unified Project Skill
- **`vinput-dev`** (`.agents/skills/vinput-dev/SKILL.md`): Architecture, Dual-planning, Fork contribution, pre-PR code health-check, Issue+PR workflow, PipeWire debugging, ecosystem extension, release packaging.
