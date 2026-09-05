#!/usr/bin/env python3
"""Generate shell completions for Full and Lite variants from templates."""

import argparse
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
TEMPLATES_DIR = PROJECT_ROOT / "data" / "completions" / "templates"
FULL_DIR = PROJECT_ROOT / "data" / "completions"
LITE_DIR = PROJECT_ROOT / "data" / "completions" / "lite"

TARGETS = [
    Path("bash/vinput"),
    Path("zsh/_vinput"),
    Path("fish/vinput.fish"),
]


def filter_completion_content(content: str, enable_local_asr: bool) -> str:
    lines = content.splitlines(keepends=True)
    out = []
    skip_local = False
    skip_lite = False
    for line in lines:
        stripped = line.strip()
        if stripped == "# @BEGIN_LOCAL_ASR@":
            skip_local = not enable_local_asr
            continue
        if stripped == "# @END_LOCAL_ASR@":
            skip_local = False
            continue
        if stripped == "# @BEGIN_LITE_ONLY@":
            skip_lite = enable_local_asr
            continue
        if stripped == "# @END_LITE_ONLY@":
            skip_lite = False
            continue
        if skip_local or skip_lite:
            continue
        out.append(line)
    return "".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate shell completions")
    parser.add_argument("--check", action="store_true", help="Check that committed files match generated output")
    args = parser.parse_args()

    for rel_path in TARGETS:
        src = TEMPLATES_DIR / rel_path
        if not src.exists():
            print(f"Error: template not found: {src}", file=sys.stderr)
            return 1
        raw = src.read_text(encoding="utf-8")

        full_content = filter_completion_content(raw, enable_local_asr=True)
        lite_content = filter_completion_content(raw, enable_local_asr=False)

        full_dst = FULL_DIR / rel_path
        lite_dst = LITE_DIR / rel_path

        if args.check:
            if not full_dst.exists() or full_dst.read_text(encoding="utf-8") != full_content:
                print(f"Error: {full_dst} is outdated. Run scripts/generate-completions.py", file=sys.stderr)
                return 1
            if not lite_dst.exists() or lite_dst.read_text(encoding="utf-8") != lite_content:
                print(f"Error: {lite_dst} is outdated. Run scripts/generate-completions.py", file=sys.stderr)
                return 1
        else:
            full_dst.parent.mkdir(parents=True, exist_ok=True)
            full_dst.write_text(full_content, encoding="utf-8")
            lite_dst.parent.mkdir(parents=True, exist_ok=True)
            lite_dst.write_text(lite_content, encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
