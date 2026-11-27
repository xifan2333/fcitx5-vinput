#!/usr/bin/env python3
"""Verify ELF symbol versions, DT_NEEDED dependencies, and payload files in a .deb package."""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
NEEDED_RE = re.compile(r"\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]")
VERSION_NAME_RE = re.compile(r"\bName:\s+(GLIBC|GLIBCXX)_([0-9]+(?:\.[0-9]+)*)\b")


def parse_version(raw: str) -> tuple[int, ...]:
    return tuple(int(part) for part in raw.split("."))


def format_version(ver: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in ver)


def is_elf_file(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with path.open("rb") as fh:
            return fh.read(4) == ELF_MAGIC
    except OSError:
        return False


def run_readelf(args: list[str], target: Path) -> str:
    env = dict(os.environ)
    env["LC_ALL"] = "C"
    proc = subprocess.run(
        ["readelf", *args, str(target)],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )
    return proc.stdout


def extract_needed(elf_path: Path) -> list[str]:
    output = run_readelf(["-d", "--wide"], elf_path)
    return NEEDED_RE.findall(output)


def extract_required_symbol_versions(elf_path: Path) -> dict[str, set[tuple[int, ...]]]:
    output = run_readelf(["-V", "--wide"], elf_path)
    versions: dict[str, set[tuple[int, ...]]] = {"GLIBC": set(), "GLIBCXX": set()}
    in_version_needs = False
    for line in output.splitlines():
        if line.startswith("Version needs section"):
            in_version_needs = True
            continue
        if in_version_needs:
            if not line.strip() or (not line.startswith(" ") and not line.startswith("\t")):
                in_version_needs = False
                continue
            match = VERSION_NAME_RE.search(line)
            if match:
                family, ver_str = match.group(1), match.group(2)
                versions[family].add(parse_version(ver_str))
    return versions


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Audit ELF ABI version ceilings and DT_NEEDED dependencies of a .deb package."
    )
    parser.add_argument("deb", type=Path, help="Path to the .deb archive to inspect")
    parser.add_argument("--max-glibc", type=str, default=None, help="Maximum allowed GLIBC version")
    parser.add_argument(
        "--max-glibcxx", type=str, default=None, help="Maximum allowed GLIBCXX version"
    )
    parser.add_argument(
        "--forbid-needed",
        action="append",
        default=[],
        help="Glob pattern for forbidden DT_NEEDED sonames (repeatable)",
    )
    parser.add_argument(
        "--require-bin",
        action="append",
        default=[],
        help="Binary name required under usr/bin/ (repeatable)",
    )
    parser.add_argument(
        "--require-file",
        action="append",
        default=[],
        help="Relative path glob required inside the package (repeatable)",
    )
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    deb_path: Path = args.deb
    if not deb_path.is_file():
        print(f"ERROR: .deb package not found: {deb_path}", file=sys.stderr)
        return 1

    max_glibc = parse_version(args.max_glibc) if args.max_glibc else None
    max_glibcxx = parse_version(args.max_glibcxx) if args.max_glibcxx else None
    violations: list[str] = []

    with tempfile.TemporaryDirectory(prefix="vinput-deb-abi-") as tmp_str:
        root = Path(tmp_str)
        subprocess.run(["dpkg-deb", "-x", str(deb_path), str(root)], check=True)

        all_files = sorted(
            p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file() or p.is_symlink()
        )

        for bin_name in args.require_bin:
            bin_path = root / "usr" / "bin" / bin_name
            if not is_elf_file(bin_path):
                violations.append(f"Missing required ELF binary: usr/bin/{bin_name}")

        for pattern in args.require_file:
            if not any(fnmatch.fnmatchcase(rel, pattern) for rel in all_files):
                violations.append(f"Missing required package file matching '{pattern}'")

        elf_files = sorted(p for p in root.rglob("*") if is_elf_file(p))
        if not elf_files:
            violations.append(f"No ELF binaries or shared libraries found in {deb_path.name}")

        print(f"Auditing {len(elf_files)} ELF file(s) in {deb_path.name}:")
        for elf_path in elf_files:
            rel = elf_path.relative_to(root).as_posix()
            needed = extract_needed(elf_path)
            for soname in needed:
                for pattern in args.forbid_needed:
                    if fnmatch.fnmatchcase(soname, pattern):
                        violations.append(
                            f"{rel}: forbidden DT_NEEDED '{soname}' (matches '{pattern}')"
                        )

            versions = extract_required_symbol_versions(elf_path)
            peak_glibc = max(versions["GLIBC"]) if versions["GLIBC"] else None
            peak_glibcxx = max(versions["GLIBCXX"]) if versions["GLIBCXX"] else None
            glibc_label = format_version(peak_glibc) if peak_glibc else "-"
            glibcxx_label = format_version(peak_glibcxx) if peak_glibcxx else "-"
            print(f"  {rel}: GLIBC={glibc_label} GLIBCXX={glibcxx_label} NEEDED={len(needed)}")

            if max_glibc and peak_glibc and peak_glibc > max_glibc:
                violations.append(
                    f"{rel}: GLIBC_{format_version(peak_glibc)} exceeds limit "
                    f"GLIBC_{format_version(max_glibc)}"
                )
            if max_glibcxx and peak_glibcxx and peak_glibcxx > max_glibcxx:
                violations.append(
                    f"{rel}: GLIBCXX_{format_version(peak_glibcxx)} exceeds limit "
                    f"GLIBCXX_{format_version(max_glibcxx)}"
                )

    if violations:
        print("\nABI gate FAILED:", file=sys.stderr)
        for item in violations:
            print(f"  - {item}", file=sys.stderr)
        return 1

    print("ABI gate PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
