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
import json
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

TRUTHY_CMAKE_VALUES = {"ON", "TRUE", "1", "YES"}


def is_elf_file(path: Path) -> bool:
    """Return True if `path` is a regular file starting with the ELF magic."""
    if not path.is_file():
        return False
    try:
        with path.open("rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def parse_cmake_cache(cache_path: Path) -> dict[str, str]:
    """Parse `KEY:TYPE=VALUE` entries from CMakeCache.txt."""
    entries: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", "//")) or "=" not in stripped:
            continue
        key_part, value = stripped.split("=", 1)
        key = key_part.split(":", 1)[0].strip()
        entries[key] = value.strip()
    return entries


def check_non_lto_debug_build(build_dir: Path) -> dict[str, str]:
    """Fail loudly unless `build_dir` is a non-LTO Debug (-O0) CMake build."""
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        raise RuntimeError(f"Missing CMakeCache.txt in {build_dir}; configure Debug build first.")

    cache = parse_cmake_cache(cache_path)
    build_type = cache.get("CMAKE_BUILD_TYPE", "")
    if build_type != "Debug":
        raise RuntimeError(
            f"{build_dir} has CMAKE_BUILD_TYPE={build_type!r}; dead-code audit requires 'Debug' (-O0)."
        )

    ipo_config_key = "CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG"
    ipo_key = ipo_config_key if ipo_config_key in cache else "CMAKE_INTERPROCEDURAL_OPTIMIZATION"
    if cache.get(ipo_key, "").upper() in TRUTHY_CMAKE_VALUES:
        raise RuntimeError(f"{build_dir} has {ipo_key}=ON; dead-code audit requires LTO disabled.")

    for flags_key in ("CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_DEBUG"):
        for flag in cache.get(flags_key, "").split():
            if flag == "-flto" or flag.startswith("-flto="):
                raise RuntimeError(
                    f"{build_dir} has {flag} in {flags_key}; dead-code audit requires LTO disabled."
                )

    return cache


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
    """Collect compiled project `.cpp.o` files from `compile_commands.json`."""
    compile_commands_path = build_dir / "compile_commands.json"
    if not compile_commands_path.is_file():
        raise RuntimeError(
            f"Missing {compile_commands_path}; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (or use a CMake preset)."
        )

    entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    objects_set: set[Path] = set()
    for entry in entries:
        output = entry.get("output")
        if not output:
            raise RuntimeError(
                f"{compile_commands_path} entry missing 'output' field; CMake >= 3.20 with Ninja/Makefiles required."
            )
        directory = Path(entry.get("directory", build_dir))
        obj = (directory / output).resolve()
        try:
            rel_obj = obj.relative_to(build_dir)
        except ValueError:
            continue
        if not rel_obj.parts or rel_obj.parts[0] != "src":
            continue
        if not obj.name.endswith(".cpp.o"):
            continue
        rel_parts = rel_obj.parts[1:]
        if any(part.endswith("_autogen") for part in rel_parts):
            continue
        if "3rdparty" in rel_parts or "_deps" in rel_parts:
            continue
        if not obj.is_file():
            raise RuntimeError(f"Missing compiled object {rel_obj}; build all targets first.")
        objects_set.add(obj)

    objects = sorted(objects_set)
    if not objects:
        raise RuntimeError(f"No compiled project .cpp.o files found in {compile_commands_path}")
    return objects


def collect_binaries(build_dir: Path, cache: dict[str, str]) -> tuple[list[Path], list[Path]]:
    """Collect production ELF binaries and active CTest ELF executables."""
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
    if (
        cache.get("BUILD_TESTING", "").upper() in TRUTHY_CMAKE_VALUES
        and (build_dir / "CTestTestfile.cmake").is_file()
    ):
        res = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
            capture_output=True,
            text=True,
            check=True,
        )
        ctest_info = json.loads(res.stdout)
        seen: set[Path] = set()
        for test in ctest_info.get("tests", []):
            cmd = test.get("command")
            if not cmd:
                continue
            exe = Path(cmd[0])
            if not exe.is_absolute():
                exe = build_dir / exe
            exe = exe.resolve()
            if exe in seen:
                continue
            seen.add(exe)
            if exe.is_relative_to(build_dir) and not exe.exists():
                raise RuntimeError(
                    f"Missing test binary {exe.relative_to(build_dir)}; build all targets first."
                )
            if is_elf_file(exe):
                test_bins.append(exe)
        test_bins.sort()

    return prod_bins, test_bins


def collect_retained_symbols(binaries: list[Path]) -> set[str]:
    """Collect retained text symbols across linked ELF binaries."""
    retained: set[str] = set()
    for binary in binaries:
        bin_syms = {
            sym for sym, sym_type in run_nm_posix(binary) if sym_type in {"T", "t", "W", "w"}
        }
        if not bin_syms:
            raise RuntimeError(
                f"No text symbols found in {binary}; binary appears stripped (unstripped Debug build required)."
            )
        retained.update(bin_syms)
    return retained


def audit_dead_code(build_dir: Path, fail_on_test_only: bool = False) -> int:
    """Run the cross-TU dead-code audit on `build_dir`."""
    build_dir = build_dir.resolve()
    cache = check_non_lto_debug_build(build_dir)
    objects = collect_project_objects(build_dir)
    prod_bins, test_bins = collect_binaries(build_dir, cache)

    defined_strong: dict[str, list[str]] = defaultdict(list)
    for obj in objects:
        rel_obj = str(obj.relative_to(build_dir))
        for sym, sym_type in run_nm_posix(obj):
            if sym_type == "T":
                defined_strong[sym].append(rel_obj)

    retained_prod = collect_retained_symbols(prod_bins)
    retained_tests = collect_retained_symbols(test_bins)
    retained_all = retained_prod | retained_tests

    dead_syms = sorted(sym for sym in defined_strong if sym not in retained_all)
    test_only_syms = sorted(
        sym for sym in defined_strong if sym not in retained_prod and sym in retained_tests
    )

    print(
        f"[check-dead-code] Audited {len(objects)} object(s), {len(defined_strong)} strong symbol(s), "
        f"{len(prod_bins)} production binary(ies), {len(test_bins)} test binary(ies)."
    )

    demangled = demangle_symbols(sorted(set(dead_syms) | set(test_only_syms)))

    if test_only_syms:
        print(
            f"[check-dead-code] Info: {len(test_only_syms)} src/ symbol(s) retained only in tests/*:"
        )
        for sym in test_only_syms:
            locs = ", ".join(defined_strong[sym])
            print(f"  - {locs}: {demangled[sym]}")

    failing = dead_syms if not fail_on_test_only else sorted(set(dead_syms) | set(test_only_syms))
    if failing:
        print(
            f"✖ [check-dead-code]: Found {len(failing)} unreferenced strong symbol(s) in src/:",
            file=sys.stderr,
        )
        for sym in failing:
            locs = ", ".join(defined_strong[sym])
            print(f"  - {locs}: {demangled[sym]} [{sym}]", file=sys.stderr)
        return 1

    print("✔ [check-dead-code]: 0 dead strong symbols across all targets.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Audit cross-TU dead strong symbols in a non-LTO Debug CMake build directory."
    )
    parser.add_argument(
        "build_dir",
        nargs="?",
        default="build",
        help="Path to non-LTO Debug CMake build directory (default: build)",
    )
    parser.add_argument(
        "--fail-on-test-only",
        action="store_true",
        help="Also fail if a src/ symbol is retained only in tests/* and not in production binaries",
    )
    args = parser.parse_args()

    try:
        return audit_dead_code(Path(args.build_dir), fail_on_test_only=args.fail_on_test_only)
    except (RuntimeError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"✖ [check-dead-code]: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
