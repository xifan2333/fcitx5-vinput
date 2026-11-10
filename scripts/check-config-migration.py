#!/usr/bin/env python3
"""Config Migration Audit Tool (fcitx5-vinput).

Enforces Hard Constraint #9 & CODING_STANDARDS.md Section 6:
If user-facing keys in `config.json` or `config.h` are modified,
a versioned migration step must be registered in
`src/common/config/config_migration.cpp` in the same PR.
"""

from __future__ import annotations

import argparse
import subprocess
import sys


def get_changed_files(base_ref: str | None = None, staged_only: bool = False) -> set[str]:
    """Retrieve list of modified files using git diff."""
    cmd = ["git", "diff", "--name-only"]
    if staged_only:
        cmd.append("--cached")
    elif base_ref:
        cmd.extend([f"{base_ref}...HEAD"])

    try:
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return {line.strip() for line in res.stdout.splitlines() if line.strip()}
    except subprocess.CalledProcessError:
        # Fall back to working tree diff if base_ref fails to resolve
        fallback = subprocess.run(
            ["git", "diff", "--name-only", "HEAD"], capture_output=True, text=True
        )
        return {line.strip() for line in fallback.stdout.splitlines() if line.strip()}


def check_config_migration(changed_files: set[str]) -> bool:
    config_sources = {
        "data/config.json",
        "src/common/config/config.h",
    }
    migration_file = "src/common/config/config_migration.cpp"

    touched_configs = config_sources.intersection(changed_files)
    if not touched_configs:
        return True

    if migration_file in changed_files:
        print(
            f"✔ [check-config-migration]: {', '.join(touched_configs)} modified with matching {migration_file}"
        )
        return True

    print(
        "✖ [check-config-migration]: Configuration definition was modified without ConfigMigration!",
        file=sys.stderr,
    )
    print(f"  Touched config files: {', '.join(touched_configs)}", file=sys.stderr)
    print(f"  Missing update in:    {migration_file}", file=sys.stderr)
    print(
        "  Rule: Breaking or structural config changes require a registered migration step in RegisteredSteps.",
        file=sys.stderr,
    )
    print(
        "  See: CODING_STANDARDS.md Section 6 & src/common/config/config_migration.cpp",
        file=sys.stderr,
    )
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit config migrations for fcitx5-vinput")
    parser.add_argument(
        "--base", default="origin/main", help="Base ref for diff comparison (default: origin/main)"
    )
    parser.add_argument("--staged", action="store_true", help="Check only staged changes")
    parser.add_argument("--files", nargs="*", help="Direct list of files (e.g. from hk/pre-commit)")
    args = parser.parse_args()

    if args.files:
        changed_files = set(args.files)
    else:
        changed_files = get_changed_files(base_ref=args.base, staged_only=args.staged)

    if not check_config_migration(changed_files):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
