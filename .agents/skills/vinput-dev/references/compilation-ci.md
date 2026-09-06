# Compilation Strategy & GitHub Actions Matrix

Compilation strategy should adapt to the local machine's hardware capabilities, prioritizing GitHub Actions CI on modest hardware.

---

## 1. Hardware-Adaptive Principle

- **Modest / Resource-Constrained Hardware (CI-First Strategy)**:
  - Avoid heavy local builds, multi-arch cross-compilation, or container runs locally.
  - Delegate compilation and tests to GitHub Actions (`ci.yml`, `release.yml`, `channels.yml`).
  - Local tasks should stay light: code editing, static formatting/linting via `hk`, and json/i18n validation.
- **High-Performance Hardware**:
  - Local incremental debug builds: `mise run dev` -> `mise run build-debug`.
  - Final release verification still uses `gh workflow run release.yml`.

---

## 2. GitHub Actions Workflows Matrix

| Workflow | File | Trigger Command | Purpose |
| :--- | :--- | :--- | :--- |
| **CI Build & Test** | `.github/workflows/ci.yml` | `gh workflow run ci.yml` | Standard build and test verification on Ubuntu runners. |
| **Release Pipeline (Dry/Formal)** | `.github/workflows/release.yml` | `gh workflow run release.yml` *(Dry)*<br>`mise run release <ver>` *(Formal tag)* | Full multi-arch (x86_64, aarch64 ARM) and multi-distro (Debian, Fedora RPM, openSUSE, Arch, Flatpak) matrix build. |
| **Packaging Channels** | `.github/workflows/channels.yml` | `mise run channels` | Builds & publishes across distribution channels (PPA, COPR, Flatpak, Debian). |
| **C++ Linter** | `.github/workflows/cpp-linter.yml` | `gh workflow run cpp-linter.yml` | PR formatting and review annotations. |
| **Nix Cache Sync** | `.github/workflows/nix-cache.yml` | `gh workflow run nix-cache.yml` | Rebuilds and pushes binaries to Cachix. |

---

## 3. The Two Usages of `release.yml`

### Mode 1: Remote Matrix Validation (Dry Run via `workflow_dispatch`)
- **Command**: `gh workflow run release.yml && gh run watch`
- **Behavior**: Compiles the code across the entire runner matrix (`ubuntu-24.04` x86_64, `ubuntu-24.04-arm` aarch64, Arch chroot, Fedora container, Flatpak builder). Uploads all build artifacts (`.deb`, `.rpm`, `.pkg.tar.zst`, `.flatpak`, source tarball) to GitHub Actions summary.
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

1. **Configuration Breaking Changes (配置破坏性更新)**:
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
