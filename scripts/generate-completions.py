#!/usr/bin/env python3
"""Generate shell completions based on local ASR support (Full vs Lite)."""

import argparse
import sys
from pathlib import Path


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
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--enable-local-asr", action="store_true", help="Enable local ASR commands")
    group.add_argument("--disable-local-asr", action="store_true", help="Disable local ASR commands")
    parser.add_argument("--src-dir", type=Path, required=True, help="Source completions directory")
    parser.add_argument("--out-dir", type=Path, required=True, help="Output completions directory")
    args = parser.parse_args()

    enable_local = args.enable_local_asr
    targets = [
        Path("bash/vinput"),
        Path("zsh/_vinput"),
        Path("fish/vinput.fish"),
    ]

    for rel_path in targets:
        src = args.src_dir / rel_path
        dst = args.out_dir / rel_path
        if not src.exists():
            print(f"Error: source file not found: {src}", file=sys.stderr)
            return 1
        dst.parent.mkdir(parents=True, exist_ok=True)
        content = src.read_text(encoding="utf-8")
        filtered = filter_completion_content(content, enable_local)
        dst.write_text(filtered, encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
