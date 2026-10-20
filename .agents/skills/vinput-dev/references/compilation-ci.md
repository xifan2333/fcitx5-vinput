# Compilation Strategy & GitHub Actions Matrix

Compilation strategy should adapt to the local machine's hardware capabilities, prioritizing GitHub Actions CI on modest hardware.

---

## 1. Hardware-Adaptive & User-Specific Principle

- **Strict Hard Constraint for User `xifan2333` (Mandatory CI-First)**:
  - If the GitHub user is `xifan2333` (or running in maintainer environment), **local compilation is STRICTLY PROHIBITED (never run `mise run build-debug` or any heavy local build commands)**.
  - Must strictly adhere to **CI-First strategy**: all build validation, tests, and matrix checks must be delegated to GitHub Actions CI (`ci.yml` / `release.yml`).
- **Modest / Resource-Constrained Hardware (CI-First Strategy)**:
  - Avoid heavy local builds, multi-arch cross-compilation, or container runs locally.
  - Delegate compilation and tests to GitHub Actions (`ci.yml`, `release.yml`, `channels.yml`).
  - Local tasks should stay light: code editing, static formatting/linting via `hk`, and json/i18n validation.
- **High-Performance Hardware (Other Users Only)**:
  - Local incremental debug builds: `mise run dev` -> `mise run build-debug` (ONLY IF the user is NOT `xifan2333`).
  - Final release verification still uses `gh workflow run release.yml`.

---

## 2. GitHub Actions Workflows Matrix

| Workflow | File | Trigger Command | Purpose |
| :--- | :--- | :--- | :--- |
| **CI Build & Test** | `.github/workflows/ci.yml` | `gh workflow run ci.yml` | Integrated pipeline: Stage 1 `cpp-linter` gate, Stage 2 matrix build (`build-linux-*`, `build-nix-lite`). |
| **Release Pipeline (Dry/Formal)** | `.github/workflows/release.yml` | `gh workflow run release.yml` *(Dry)*<br>`mise run release <ver>` *(Formal tag)* | Full multi-arch (x86_64, aarch64 ARM) and multi-distro (Debian, Fedora RPM, openSUSE, Arch, Flatpak) matrix build. |
| **Packaging Channels** | `.github/workflows/channels.yml` | `mise run channels` | Builds & publishes across distribution channels (PPA, COPR, Flatpak, Debian). |
| **Linter PR Commenter** | `.github/workflows/cpp-linter-comment.yml` | `workflow_run` (automatic) | Posts and updates Clang-Format & Clang-Tidy quality comments on PRs. |
| **Nix Cache Sync** | `.github/workflows/nix-cache.yml` | `gh workflow run nix-cache.yml` | Rebuilds and pushes binaries to Cachix. |

---

## 3. The Two Usages of `release.yml`

### Mode 1: Remote Matrix Validation (Dry Run via `workflow_dispatch`)
- **Command**: `gh workflow run release.yml --ref <branch_name> && gh run watch`
- **Behavior**: Compiles the code across the entire runner matrix (`ubuntu-24.04` x86_64, `ubuntu-24.04-arm` aarch64, Arch chroot, Fedora container, Flatpak builder) for the specified branch. Uploads all build artifacts (`.deb`, `.rpm`, `.pkg.tar.zst`, `.flatpak`, source tarball) to GitHub Actions summary.
- **Safety**: Does **NOT** publish a GitHub Release because the `publish-release` job requires a `refs/tags/v*` ref.
- **When to use**: Whenever you want to test whether all architectures and packaging targets compile cleanly before tagging.

### Mode 2: Official Release Publication (Git Tag `v*`)
- **Command**: `mise run release <version>` (e.g. `mise run release 2.3.9`)
- **Behavior**: Verifies that the `VERSION` file matches the git tag, runs the complete matrix build, extracts changelog notes via `git-cliff`, and automatically creates and publishes the official GitHub Release with all binary packages attached.

---

## 4. ConfigMigration for Breaking Schema Changes

When a feature or refactor **incompatibly** changes user config (`~/.config/vinput/config.json` or `~/.config/fcitx5/conf/vinput.conf`):

1. Add a versioned step in `src/common/config/config_migration.cpp` (`RegisteredSteps`) in the **same PR** as the schema change.
2. Use helpers: `RenameField`, `EnsureField` / `EnsureFieldInArray`, `ReplaceIniKey`, `RemoveIniKey`.
3. Do **not** keep legacy field aliases in runtime parsers.
4. Users and agents upgrade with `vinput config migrate` (`--dry-run` to preview). Backups go under `backups/`.

The engine is the source of truth. `notification.json` only teaches the command; it must not contain per-key rename recipes.

## 5. In-App Release Notification Specification (`notification.json`)

When publishing releases, `notification.json` triggers startup notifications in the GUI:

1. **Configuration Breaking Changes**:
   - Schema work belongs in ConfigMigration (section 4), not in this file. `notification.json` only teaches how to run the tool.
   - **Content Format: "Prompt for Agent"**: An actionable prompt that humans and agents can execute directly:
     - **Target Version Boundary**: Explicitly state the starting version (e.g. `【Target: Upgrading to vX.Y.Z or newer】`).
     - **Option 1 (Fast Reset)**: One-liner `vinput init -f` to regenerate default configs.
     - **Option 2 (In-place migrate)**: `vinput config migrate --dry-run` to preview, then `vinput config migrate` to apply (automatic backups under `backups/`). Do **not** list per-key rename/addition recipes in the notification.
   - **Retention Period (5 Patch Releases or Next Minor)**:
     - Retain the breaking change migration prompt as the primary notification body across **5 consecutive patch releases** (or until the next minor release, e.g. `v2.4.0`).
     - During this retention window, intermediate pure feature/bugfix releases should retain the migration prompt and append a concise patch changelog at the end. This prevents users who skip patch versions from missing critical migration instructions.
2. **Standard Releases (Non-Breaking Changes)**:
   - Record version highlights and bug fixes concisely in bilingual (`en_US` and `zh_CN`) format.

## 6. Semantic Versioning & Release Cadence (Skill-Based Soft Governance)

Releases strictly adhere to Semantic Versioning (`MAJOR.MINOR.PATCH`) via `vinput-dev` skill guidelines:

| Level | Code Scope & Changes | Requirement & Example |
| :--- | :--- | :--- |
| **MAJOR (X)** | Breaking D-Bus method removals/signature alterations, CLI subcommand/option/alias removals, unmigratable config overhaul, or explicit `BREAKING CHANGE:` / `feat!:` / `fix!:` commits. | Bump `X.0.0` (e.g. `3.0.0`). |
| **MINOR (Y)** | Configuration schema changes (`core_config_types.h`, `vinput_config.h`, `default-config.json`, or added steps in `config_migration.cpp`), new CLI commands/options/aliases, added D-Bus methods/signals, new features (`feat:` commits), or GUI module expansions. | Bump `x.Y.0` (e.g. `2.4.0`). **Breaking config changes must NEVER be released as a PATCH!** |
| **PATCH (Z)** | Pure bugfixes (`fix:`), refactoring, doc updates, and CI improvements without schema, CLI, or API contract changes. | Bump `x.y.Z` (e.g. `2.3.27`). |

---

## 7. CI Quota Governance & Proactive Cancellation Protocol

### 1. Stage Gate Auto-Short-Circuiting
The primary CI pipeline (`.github/workflows/ci.yml`) enforces a two-stage gate:
1. **Stage 1 (`cpp-linter`)**: Runs fast Clang-Format and Clang-Tidy checks (~1.5 min).
2. **Stage 2 (`build` & `build-nix-lite`)**: Heavy compilation jobs declare `needs: [cpp-linter]`.
If formatting or static analysis fails, **downstream compilation jobs are immediately Skipped**, saving ~15 minutes of runner compute per failed run.

### 2. Proactive Run Inspection & Cancellation
Guard runner quotas by canceling obsolete runs before pushing follow-up commits (never cancel runs on `main`):

```bash
# 1. Inspect active runs for current feature branch
gh run list --branch $(git branch --show-current) --status in_progress

# 2. Cancel a specific run by ID
gh run cancel <run_id>

# 3. Cancel obsolete runs while preserving current HEAD (if run after push)
gh run list --branch $(git branch --show-current) --status in_progress --json databaseId,headSha --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" | xargs -r -n 1 gh run cancel
```

**Governance Approach**:
- **Soft Constraint via Skill**: Semantic compatibility (D-Bus contract preservation, CLI options/subcommands, and configuration migration) is governed by the `vinput-dev` skill and agent review loops, avoiding brittle shell-based AST regex hooks.
- **Cadence Discipline**: Accumulate verified fixes on `main` and run CI dry builds (`release.yml`) before cutting tags. Avoid hasty releases for isolated micro-fixes.
