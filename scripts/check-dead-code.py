#!/usr/bin/env python3
"""Cross-TU Dead-Code Link Gate (fcitx5-vinput).

Audits a non-LTO (-O0 / Debug) CMake build directory by comparing strong
external-linkage text symbols (`T`) defined in project `src/` object files
against all retained text symbols (`T`/`t`/`W`/`w`) across every linked
production binary and unit test executable.

Any strong symbol defined in `src/` that is retained in zero final binaries
(neither production targets nor tests) is dead across all translation units
and fails the gate.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

PRODUCTION_BINARIES = (
    "src/cli/vinput",
    "src/daemon/vinput-daemon",
    "src/gui/vinput-gui",
    "src/addon/fcitx5-vinput.so",
)


def is_elf_file(path: Path) -> bool:
    """Return True if `path` is a regular file starting with the ELF magic."""
    if not path.is_file():
        return False
    try:
        with path.open("rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def check_non_lto_build(build_dir: Path) -> None:
    """Fail loudly if CMakeCache.txt indicates an LTO or optimized build."""
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        return

    cache_text = cache_path.read_text(encoding="utf-8", errors="replace")
    build_type = ""
    ipo_release = ""
    ipo_relwithdebinfo = ""
    for line in cache_text.splitlines():
        if line.startswith("CMAKE_BUILD_TYPE:"):
            build_type = line.split("=", 1)[1].strip()
        elif line.startswith("CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE:"):
            ipo_release = line.split("=", 1)[1].strip()
        elif line.startswith("CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO:"):
            ipo_relwithdebinfo = line.split("=", 1)[1].strip()

    if build_type == "Release" and ipo_release.upper() in {"ON", "TRUE", "1"}:
        raise RuntimeError(
            f"{build_dir} is a Release LTO build; dead-code audit requires a non-LTO Debug (-O0) build."
        )
    if build_type == "RelWithDebInfo" and ipo_relwithdebinfo.upper() in {"ON", "TRUE", "1"}:
        raise RuntimeError(
            f"{build_dir} is a RelWithDebInfo LTO build; dead-code audit requires a non-LTO Debug (-O0) build."
        )


def run_nm_posix(path: Path) -> list[tuple[str, str]]:
    """Return `(symbol_name, symbol_type)` pairs from `nm --defined-only --format=posix`."""
    res = subprocess.run(
        ["nm", "--defined-only", "--format=posix", str(path)],
        capture_output=True,
        text=True,
        check=True,
    )
    entries: list[tuple[str, str]] = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            entries.append((parts[0], parts[1]))
    return entries


def demangle_symbols(symbols: list[str]) -> dict[str, str]:
    """Demangle C++ symbol names in batch via `c++filt`."""
    if not symbols:
        return {}
    res = subprocess.run(
        ["c++filt"],
        input="\n".join(symbols) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    demangled = res.stdout.splitlines()
    if len(demangled) != len(symbols):
        return {sym: sym for sym in symbols}
    return dict(zip(symbols, demangled, strict=True))


def collect_project_objects(build_dir: Path) -> list[Path]:
    """Collect compiled project `.cpp.o` files under `<build_dir>/src/`."""
    src_build = build_dir / "src"
    if not src_build.is_dir():
        raise RuntimeError(f"Missing build src directory: {src_build}")

    objects: list[Path] = []
    for obj in sorted(src_build.rglob("*.cpp.o")):
        rel_parts = obj.relative_to(src_build).parts
        if any(part.endswith("_autogen") for part in rel_parts):
            continue
        if "3rdparty" in rel_parts or "_deps" in rel_parts:
            continue
        objects.append(obj)

    if not objects:
        raise RuntimeError(f"No compiled project .cpp.o files found under {src_build}")
    return objects


def collect_binaries(build_dir: Path) -> tuple[list[Path], list[Path]]:
    """Collect production ELF binaries and test ELF executables."""
    prod_bins: list[Path] = []
    missing: list[str] = []
    for rel in PRODUCTION_BINARIES:
        candidate = build_dir / rel
        if is_elf_file(candidate):
            prod_bins.append(candidate)
        else:
            missing.append(rel)

    if missing:
        raise RuntimeError(f"Missing production ELF binaries in {build_dir}: {', '.join(missing)}")

    test_bins: list[Path] = []
    tests_dir = build_dir / "tests"
    if tests_dir.is_dir():
        for entry in sorted(tests_dir.iterdir()):
            if is_elf_file(entry):
                test_bins.append(entry)

    return prod_bins, test_bins


def audit_dead_code(build_dir: Path, fail_on_test_only: bool = False) -> int:
    """Run the cross-TU dead-code audit on `build_dir`."""
    check_non_lto_build(build_dir)
    objects = collect_project_objects(build_dir)
    prod_bins, test_bins = collect_binaries(build_dir)

    defined_strong: dict[str, list[str]] = defaultdict(list)
    for obj in objects:
        rel_obj = str(obj.relative_to(build_dir))
        for sym, sym_type in run_nm_posix(obj):
            if sym_type == "T":
                defined_strong[sym].append(rel_obj)

    retained_prod: set[str] = set()
    for binary in prod_bins:
        for sym, sym_type in run_nm_posix(binary):
            if sym_type in {"T", "t", "W", "w"}:
                retained_prod.add(sym)

    retained_tests: set[str] = set()
    for binary in test_bins:
        for sym, sym_type in run_nm_posix(binary):
            if sym_type in {"T", "t", "W", "w"}:
                retained_tests.add(sym)

    retained_all = retained_prod | retained_tests
    dead_syms = sorted(sym for sym in defined_strong if sym not in retained_all)
    test_only_syms = sorted(
        sym for sym in defined_strong if sym not in retained_prod and sym in retained_tests
    )

    print(
        f"[check-dead-code] Audited {len(objects)} object(s), {len(defined_strong)} strong symbol(s), "
        f"{len(prod_bins)} production binary(ies), {len(test_bins)} test binary(ies)."
    )

    if test_only_syms:
        demangled_test_only = demangle_symbols(test_only_syms)
        print(
            f"[check-dead-code] Info: {len(test_only_syms)} src/ symbol(s) retained only in tests/*:"
        )
        for sym in test_only_syms:
            locs = ", ".join(defined_strong[sym])
            print(f"  - {locs}: {demangled_test_only[sym]}")

    if dead_syms or (fail_on_test_only and test_only_syms):
        failing = (
            dead_syms if not fail_on_test_only else sorted(set(dead_syms) | set(test_only_syms))
        )
        demangled_dead = demangle_symbols(failing)
        print(
            f"✖ [check-dead-code]: Found {len(failing)} unreferenced strong symbol(s) in src/:",
            file=sys.stderr,
        )
        for sym in failing:
            locs = ", ".join(defined_strong[sym])
            print(f"  - {locs}: {demangled_dead[sym]} [{sym}]", file=sys.stderr)
        return 1

    print("✔ [check-dead-code]: 0 dead strong symbols across all targets.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Audit cross-TU dead strong symbols in a non-LTO CMake build directory."
    )
    parser.add_argument(
        "build_dir",
        nargs="?",
        default="build",
        help="Path to non-LTO CMake build directory (default: build)",
    )
    parser.add_argument(
        "--fail-on-test-only",
        action="store_true",
        help="Also fail if a src/ symbol is retained only in tests/* and not in production binaries",
    )
    args = parser.parse_args()

    try:
        return audit_dead_code(Path(args.build_dir), fail_on_test_only=args.fail_on_test_only)
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"✖ [check-dead-code]: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
