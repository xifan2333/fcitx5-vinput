# Playbook: Packaging, CI Validation & Release

The canonical execution workflow for multi-architecture builds, packaging channel dry runs, and SemVer release tagging.

> **Execution Contract**:
> Copy these steps verbatim into your task checklist (Todo List). Strictly honor the maintainer CI-first rule.

---

## Steps

1. **Hardware-Adaptive Gate**:
   - Check current user / maintainer environment:
     - If the GitHub user is `xifan2333`: **LOCAL COMPILATION IS STRICTLY PROHIBITED (never run `mise run build-debug` or CMake builds)**. Prioritize GitHub Actions CI exclusively.
     - For other users on modest hardware: also prioritize GitHub Actions CI over local heavy builds.

2. **Trigger Standard PR CI**:
   - For pull request verification:
     ```bash
     gh workflow run ci.yml --ref <branch-name>
     ```

3. **Remote Matrix Dry Run (Multi-Arch & Multi-Distro)**:
   - To validate packaging across all distributions without publishing:
     ```bash
     gh workflow run release.yml --ref <branch-name>
     ```
   - This compiles the full matrix (Ubuntu x86_64, Ubuntu ARM aarch64, Arch chroot, Fedora container, Flatpak builder) and uploads build artifacts (`.deb`, `.rpm`, `.pkg.tar.zst`, `.flatpak`) to Actions artifacts.
   - It does NOT publish a release because release publication requires a `refs/tags/v*` tag.

4. **Packaging Channels Sync (PPA / COPR / Flatpak)**:
   - Test packaging channels workflow:
     ```bash
     mise run channels <ref> [version]
     ```

5. **SemVer & Breaking Change Audit**:
   - If configuration schema changed, verify that version bump is a MINOR bump (`x.Y.0`), never a PATCH (`x.y.Z`).
   - Verify that `src/common/config/config_migration.cpp` contains the registered migration step (`mise run check:migration`).

6. **Proactive CI Quota Cancellation**:
   - Before pushing new commits or switching tasks, cancel stale runs:
     ```bash
     gh run list --branch $(git branch --show-current) --status in_progress --json databaseId,headSha --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" | xargs -r -n 1 gh run cancel
     ```
   - *Red Line*: Never cancel runs on `main`.
