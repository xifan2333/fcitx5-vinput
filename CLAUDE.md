# Claude Code Guide

See [AGENTS.md](AGENTS.md) for full architecture, dual-planning model, compilation strategies, Issue+PR workflow, and hard constraints.

## Quick CLI Reference

- **Dual-Planning Model**:
  - *Phase A (Task Planning - BEFORE CODING)*:
    `gh issue view <id>` -> `git checkout -b <type>/issue-<id>-<desc>` -> `git commit --allow-empty -m "chore: init draft pr"` -> `git push -u origin <branch>` -> `gh pr create --draft --body "Closes #<id>\n\n### Tasks\n- [ ] 1. ..."`
  - *Phase B (Quality Gate Pre-check - AFTER EDIT)*: `mise run check:plan`
- **Execution Loop (ONE ITEM AT A TIME)**:
  - Code task N -> `mise run check:changed` (or `hk fix`) -> atomic commit & push -> `gh pr edit --body` (update task N to `- [x]`)
  - Structured fix stream: `hk run check --safe --format jsonl`
  - Finish: `gh pr ready && gh pr merge --squash --delete-branch`
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

## Ecosystem Repositories
- **Core Engine (C++20)**: [xifan2333/fcitx5-vinput](https://github.com/xifan2333/fcitx5-vinput) (`~/Code/fcitx5-vinput`)
- **Cloud ASR / LLM Scenes Registry**: [xifan2333/vinput-registry](https://github.com/xifan2333/vinput-registry) (`~/Code/vinput-registry`)
- **Arch AUR Packaging Automation**: [xifan2333/aur-auto](https://github.com/xifan2333/aur-auto) (`~/Code/aur-auto`)
- **Flatpak OSTree Repository Automation**: [xifan2333/flatpak-auto](https://github.com/xifan2333/flatpak-auto) (`~/Code/flatpak-auto`)

## Unified Project Skill
- **`vinput-dev`** (`.agents/skills/vinput-dev/SKILL.md`): Architecture, Dual-planning, Fork contribution, pre-PR code health-check, Issue+PR workflow, PipeWire debugging, ecosystem extension, release packaging.
