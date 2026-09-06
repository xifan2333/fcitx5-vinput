# fcitx5-vinput Agent Guide

Guidelines, dual-planning model, and hard constraints for AI coding agents working on `fcitx5-vinput`.

---

## 1. Project Overview & Architecture

`fcitx5-vinput` is a voice input system for Fcitx5 providing local (sherpa-onnx) and cloud ASR, LLM post-processing, and cross-distro packaging.

- `src/addon/`: Fcitx5 input method addon (Qt/C++, hotkey triggers like `Alt_R` / `Shift_R`, push-to-talk, D-Bus client).
- `src/daemon/`: Core daemon (`vinput-daemon`), handles PipeWire audio recording, sherpa-onnx inference, cloud ASR engines, LLM scene transformations, D-Bus service (`org.fcitx.Vinput`).
- `src/cli/`: Standalone `vinput` CLI for manual recording, profile switching, and status inspection.
- `src/common/`: Shared types, configuration structs (`nlohmann-json`), D-Bus XML interfaces.
- `po/` & `i18n/`: Translations (Gettext and Qt `.ts` / `.qm`).

---

## 2. Related Repositories & Ecosystem

`fcitx5-vinput` is part of a multi-repository ecosystem:

| Repository | GitHub URL | Local Path | Role & Purpose |
| :--- | :--- | :--- | :--- |
| **`fcitx5-vinput`** (Core) | [xifan2333/fcitx5-vinput](https://github.com/xifan2333/fcitx5-vinput) | `.` (`~/Code/fcitx5-vinput`) | Main C++20 repository: Fcitx5 addon, background daemon, CLI, PipeWire capture, local sherpa-onnx runtime, D-Bus service, GitHub releases. |
| **`vinput-registry`** | [xifan2333/vinput-registry](https://github.com/xifan2333/vinput-registry) | `~/Code/vinput-registry` | Resource catalog: index for local ASR models (`models.json`), cloud ASR provider scripts (`providers.json` + `resources/providers/`), and LLM scene adapters (`adapters.json` + `resources/adapters/`). |
| **`aur-auto`** | [xifan2333/aur-auto](https://github.com/xifan2333/aur-auto) | `~/Code/aur-auto` | Arch User Repository (AUR) automation: tracks `fcitx5-vinput` releases via `pkgs/fcitx5-vinput-bin/`, tests in clean chroot, and publishes to AUR. |
| **`flatpak-auto`** | [xifan2333/flatpak-auto](https://github.com/xifan2333/flatpak-auto) | `~/Code/flatpak-auto` | Flatpak repository automation: tracks releases via `products/fcitx5-vinput/`, imports bundles into shared OSTree repo, and publishes `.flatpakref` / `.flatpakrepo` to GitHub Pages. |

- **Adding / Modifying Cloud ASR or LLM Scenes**: Work in `~/Code/vinput-registry`.
- **Packaging / AUR Release Tracking**: Work in `~/Code/aur-auto` (`pkgs/fcitx5-vinput-bin/`).
- **Flatpak Distribution & OSTree Sync**: Work in `~/Code/flatpak-auto` (`products/fcitx5-vinput/`).

For detailed operational guides, fork contribution, code health check SOPs, and cross-repo workflows, refer to the unified skill at `.agents/skills/vinput-dev/SKILL.md`.

---

## 3. Dual-Planning Model for AI Agents

To avoid ambiguity between functional task planning and toolchain validation, agents must distinguish between two distinct planning phases:

| Phase | Concept & Terminology | Timing | Tool & Output | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **Phase A** | **Task Planning**<br>*(Feature / Bugfix Breakdown)* | **Pre-development**<br>*(Before coding)* | GitHub Issue & Draft PR body (`- [ ]` checklist) | Defines *what* code to write, module boundaries, and task sequencing. |
| **Phase B** | **Quality Gate Pre-check**<br>*(hk --plan)* | **Post-edit**<br>*(Before committing)* | `mise run check:plan`<br>*(or `hk run check --safe --plan`)* | Previews *which* linters/formatters will run and their side-effects on edited files. |

---

## 4. Strict Chronological Development Workflow (Issue + Draft PR)

All coding agents must strictly operate within this closed-loop chronological lifecycle:

```
+-------------------------------------------------------------+
| 1. Pre-Code Initialization (MANDATORY BEFORE ANY CODE)      |
|    gh issue view <id>                                       |
|    git checkout -b <type>/issue-<id>-<desc>                 |
|    git commit --allow-empty -m "chore: init draft pr..."    |
|    git push -u origin <branch>                              |
|    gh pr create --draft (ALL items unchecked: - [ ])        |
+------------------------------+------------------------------+
                               |
                +--------------v--------------+
                | 2. Code ONLY Task N         |
                |    Focus ONLY on first - [ ]|
                +--------------+--------------+
                               |
                +--------------v--------------+
                | 3. Quality Gate & Pre-check |
                |    mise run check:plan      |
                |    mise run check:changed   |
                |    hk fix (if formatting)   |
                +--------------+--------------+
                               |
                +--------------v--------------+
                | 4. Local Atomic Commit      |
                |    git commit -m "feat:..." |
                |    (Keep commit local)      |
                +--------------+--------------+
                               | (Remaining tasks?)
                               +---------- Yes ---------+
                               | No                     |
+------------------------------v--------------+         |
| 5. Unified Push, Checks & Merge             |         |
|    git push origin <branch>                 |         |
|    gh pr edit --body (check all - [x])      |         |
|    gh pr checks (verify CI passed)          |         |
|    gh pr ready                              |         |
|    gh pr merge --squash --delete-branch     |         |
+---------------------------------------------+         |
                               ^                         |
                               +-------------------------+
```

### Phase 1: Pre-Code Initialization (MANDATORY BEFORE ANY CODING)
1. Inspect the issue requirements: `gh issue view <issue_id>`
2. Create a clean feature branch: `git checkout -b <type>/issue-<id>-<short-description>`
3. Push an initial empty commit to open the Draft PR immediately:
   ```bash
   git commit --allow-empty -m "chore: initialize draft pr for issue #<issue_id>"
   git push -u origin <type>/issue-<id>-<short-description>
   ```
4. Create the **Draft PR** containing the structured implementation checklist with ALL items UNCHECKED (`- [ ]`):
   ```bash
   gh pr create --draft \
     --title "<type>: <description> (#<issue_id>)" \
     --body "Closes #<issue_id>

   ### Implementation Tasks
   - [ ] 1. Core data structures in src/common/
   - [ ] 2. Implement logic in src/daemon/
   - [ ] 3. Update i18n and tests"
   ```
   *(At this stage, the PR progress bar shows: 0 of 3 tasks completed)*

### Phase 2: Iterative Execution Loop (ONE TASK AT A TIME)
Repeat for each unchecked `- [ ]` item:
1. **Single-Item Code**: Focus ONLY on the topmost unchecked task (`- [ ]`). Do not modify unrelated files.
2. **Quality Gate Pre-check**:
   - Preview matched rules: `mise run check:plan`
   - Run safe validation on changed files: `mise run check:changed`
   - Automated Fix Loops (`--format jsonl`):
     ```bash
     hk run check --safe --format jsonl
     ```
   - Auto-format if needed: `hk fix` (or `mise run fix`).
   - Validate translations: `python3 scripts/check-i18n.py`.
   - If local machine hardware permits: `mise run build-debug`. If resource-constrained, rely on CI.
3. **Local Atomic Commit**:
   Keep commits strictly atomic (one commit per `- [ ]` task), but keep them local during intermediate steps to avoid triggering redundant, cancelled CI runs:
   ```bash
   git add <modified_files>
   git commit -m "<type>(<scope>): <concise message> (#<issue_id>)"
   ```

### Phase 3: Final Validation, Unified Push & Merge
1. Once all checklist tasks are locally completed and committed:
   ```bash
   git push origin <branch_name>
   ```
2. Update the Draft PR body to check off all completed items (`- [x]`):
   ```bash
   gh pr edit --body "..."
   ```
3. Verify PR CI checks: `gh pr checks`.
4. (Optional for core changes) Trigger remote matrix dry build: `gh workflow run release.yml && gh run watch`.
5. Mark PR ready and squash-merge: `gh pr ready && gh pr merge --squash --delete-branch`.

---

## 5. Compilation Strategy: Hardware-Adaptive (CI-First on Modest Hardware)

Compilation strategy should adapt to local hardware capabilities:

- **Modest / Resource-Constrained Local Hardware -> CI-First Strategy**:
  - Do NOT run heavy local full builds, multi-arch cross-compilations, or container builds locally.
  - Rely on **GitHub Actions CI** for building, testing, and matrix validation:
    - Standard CI: `gh workflow run ci.yml && gh run watch`
    - Pre-release full matrix dry run: `gh workflow run release.yml && gh run watch`
    - Packaging channels: `mise run channels`
  - Local operations should stay lightweight: code editing, static formatting/linting via `hk`, and json/i18n validation.
- **High-Performance Hardware with Full Toolchains**:
  - Local incremental debug builds are permitted: `mise run dev` -> `mise run build-debug`.
  - Always run `gh workflow run release.yml` before cutting a release for clean-room multi-distro verification.

---

## 6. Agent Hard Constraints (Red Lines)

1. **No Direct Main Commits**: Never push implementation code directly to `main`.
2. **Draft PR Pre-Creation**: Draft PR with unchecked items (`- [ ]`) MUST be created before writing implementation code.
3. **No Multi-Task Lump Commits**: Every commit must map to exactly one `- [ ]` task in the PR checklist.
4. **No Push Without Quality Gate**: Never push code before running `mise run check:changed` or `hk run check --safe`.
5. **Transparent Progress Tracking**: When asked for status, agents must report progress based on the PR checklist (e.g., "Completed 2 of 4 tasks; currently implementing task 3").
6. **Hardware-Adaptive Compilation**: On modest hardware, prioritize GitHub Actions CI (`ci.yml` / `release.yml`) over heavy local full builds.
7. **User-Facing Strings**: Must be wrapped in `_("...")` or `ki18n` for gettext localization. Run `mise run check-i18n` to validate po files.
8. **No Force-Pushing to Contributor Forks (Open-Source Etiquette)**: Never force-push (`git push -f`) to an external contributor's personal fork or PR branch, even if GitHub's "Allow edits by maintainers" is technically enabled. Overwriting a contributor's commit history breaks their local workspace and violates open-source collaboration boundaries. When a contributor's PR encounters conflicts (e.g., following an earlier PR merge), either politely ask the contributor to rebase via a PR comment, or integrate the changes purely within upstream local/temporary branches without modifying the contributor's remote repository.
