#!/usr/bin/env python3
"""Detect missing, obsolete, and untranslated strings in .po and .ts files."""

import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent

PO_FILE = PROJECT_ROOT / "po" / "zh_CN.po"
TS_FILE = PROJECT_ROOT / "i18n" / "vinput-gui_zh_CN.ts"

SRC_DIRS = ["src/cli", "src/common", "src/addon", "src/daemon"]

# Colors (disabled when not a terminal)
if sys.stdout.isatty():
    RED, YELLOW, CYAN, RESET = "\033[0;31m", "\033[0;33m", "\033[0;36m", "\033[0m"
else:
    RED = YELLOW = CYAN = RESET = ""


def extract_po_msgids(path: str) -> set[str]:
    """Extract msgid strings from a .po/.pot file (skip header and obsolete #~)."""
    ids = set()
    with open(path, encoding="utf-8") as f:
        text = f.read()
    for entry in re.split(r"\n\n+", text):
        lines = [line.strip() for line in entry.splitlines() if line.strip()]
        if not lines or lines[0].startswith("#~"):
            continue
        msgid_parts = []
        in_msgid = False
        for line in lines:
            if line.startswith("msgid "):
                in_msgid = True
                m = re.match(r'^msgid\s+"(.*)"$', line)
                if m:
                    msgid_parts.append(m.group(1).encode().decode("unicode_escape", "ignore"))
            elif in_msgid and line.startswith('"') and line.endswith('"'):
                m = re.match(r'^"(.*)"$', line)
                if m:
                    msgid_parts.append(m.group(1).encode().decode("unicode_escape", "ignore"))
            elif line.startswith("msgstr "):
                in_msgid = False
        full_id = "".join(msgid_parts)
        if full_id:
            ids.add(full_id)
    return ids


def extract_po_obsolete_msgids(path: str) -> set[str]:
    """Extract #~ msgid strings from a .po file."""
    ids = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'^#~\s*msgid "(.*)"$', line)
            if m and m.group(1):
                ids.add(m.group(1))
    return ids


def extract_po_untranslated(path: str) -> set[str]:
    """Find non-obsolete msgids with empty msgstr."""
    ids = set()
    with open(path, encoding="utf-8") as f:
        text = f.read()
    for entry in re.split(r"\n\n+", text):
        lines = [line.strip() for line in entry.splitlines() if line.strip()]
        if not lines or lines[0].startswith("#~"):
            continue
        msgid_parts = []
        msgstr_parts = []
        in_msgid = False
        in_msgstr = False
        for line in lines:
            if line.startswith("msgid "):
                in_msgid = True
                in_msgstr = False
                m = re.match(r'^msgid\s+"(.*)"$', line)
                if m:
                    msgid_parts.append(m.group(1).encode().decode("unicode_escape", "ignore"))
            elif in_msgid and line.startswith('"') and line.endswith('"'):
                m = re.match(r'^"(.*)"$', line)
                if m:
                    msgid_parts.append(m.group(1).encode().decode("unicode_escape", "ignore"))
            elif line.startswith("msgstr "):
                in_msgid = False
                in_msgstr = True
                m = re.match(r'^msgstr\s+"(.*)"$', line)
                if m:
                    msgstr_parts.append(m.group(1).encode().decode("unicode_escape", "ignore"))
            elif in_msgstr and line.startswith('"') and line.endswith('"'):
                m = re.match(r'^"(.*)"$', line)
                if m:
                    msgstr_parts.append(m.group(1).encode().decode("unicode_escape", "ignore"))
        full_id = "".join(msgid_parts)
        full_str = "".join(msgstr_parts)
        if full_id and not full_str.strip():
            ids.add(full_id)
    return ids


def ts_sources(path: Path, *, skip_vanished: bool = False) -> set[str]:
    """Extract <source> texts from a .ts file via XML parser."""
    tree = ET.parse(path)
    result = set()
    for msg in tree.iter("message"):
        if skip_vanished:
            tr = msg.find("translation")
            if tr is not None and tr.get("type") in ("vanished", "obsolete"):
                continue
        src = msg.find("source")
        if src is not None and src.text:
            result.add(src.text)
    return result


def ts_vanished(path: Path) -> set[str]:
    """Extract vanished/obsolete source texts from a .ts file."""
    tree = ET.parse(path)
    result = set()
    for msg in tree.iter("message"):
        tr = msg.find("translation")
        if tr is not None and tr.get("type") in ("vanished", "obsolete"):
            src = msg.find("source")
            if src is not None and src.text:
                result.add(src.text)
    return result


def ts_untranslated(path: Path) -> set[str]:
    """Find non-vanished entries with empty translation."""
    tree = ET.parse(path)
    result = set()
    for msg in tree.iter("message"):
        tr = msg.find("translation")
        if tr is None:
            continue
        if tr.get("type") in ("vanished", "obsolete"):
            continue
        if not tr.text or not tr.text.strip():
            src = msg.find("source")
            if src is not None and src.text:
                result.add(src.text)
    return result


def find_lupdate() -> str | None:
    for cmd in ("lupdate-qt6", "lupdate6", "lupdate"):
        if subprocess.run(["which", cmd], capture_output=True).returncode == 0:
            return cmd
    return None


def main() -> int:
    po_missing = po_obsolete = po_untranslated = 0
    ts_missing_n = ts_vanished_n = ts_untranslated_n = 0

    # === Gettext (po) ===
    print(f"{CYAN}=== Gettext (po) ==={RESET}")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_pot = os.path.join(tmpdir, "extracted.pot")

        # Collect source files
        src_files = []
        for d in SRC_DIRS:
            p = PROJECT_ROOT / d
            if p.is_dir():
                src_files.extend(
                    sorted(
                        str(f)
                        for f in p.rglob("*")
                        if f.suffix in (".cpp", ".h")
                    )
                )

        subprocess.run(
            [
                "xgettext",
                "--keyword=_",
                "--keyword=N_",
                "--language=C++",
                "--from-code=UTF-8",
                "--no-location",
                "--sort-by-file",
                "-o",
                tmp_pot,
                *src_files,
            ],
            check=True,
            capture_output=True,
        )

        pot_ids = extract_po_msgids(tmp_pot)
        po_ids = extract_po_msgids(str(PO_FILE))

        # Missing: in source but not in .po
        for mid in sorted(pot_ids - po_ids):
            print(f"{RED}MISSING:{RESET} {mid}")
            po_missing += 1

        # Obsolete: in .po but no longer in source, or marked #~
        obsolete_ids = (po_ids - pot_ids) | extract_po_obsolete_msgids(str(PO_FILE))
        for mid in sorted(obsolete_ids):
            print(f"{YELLOW}OBSOLETE:{RESET} {mid}")
            po_obsolete += 1

        # Untranslated
        for mid in sorted(extract_po_untranslated(str(PO_FILE))):
            print(f"{YELLOW}UNTRANSLATED:{RESET} {mid}")
            po_untranslated += 1

    print()

    # === Qt (ts) ===
    print(f"{CYAN}=== Qt (ts) ==={RESET}")

    lupdate = find_lupdate()
    if lupdate is None:
        print("WARNING: lupdate not found, skipping Qt .ts checks")
        print()
        print(f"{CYAN}=== Summary ==={RESET}")
        print(
            f"         {po_missing} missing, {po_obsolete} obsolete, "
            f"{po_untranslated} untranslated (po)"
        )
        print("         (ts skipped — lupdate not available)")
        return 1 if (po_missing + po_obsolete) > 0 else 0

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_ts = os.path.join(tmpdir, "extracted.ts")
        gui_src = str(PROJECT_ROOT / "src" / "gui")

        subprocess.run(
            [lupdate, gui_src, "-ts", tmp_ts, "-no-obsolete"],
            capture_output=True,
        )

        ext_srcs = ts_sources(Path(tmp_ts))
        cur_srcs = ts_sources(TS_FILE, skip_vanished=True)

        # Missing
        for s in sorted(ext_srcs - cur_srcs):
            print(f'{RED}MISSING:{RESET} "{s}"')
            ts_missing_n += 1

        # Vanished
        for s in sorted(ts_vanished(TS_FILE)):
            print(f'{YELLOW}VANISHED:{RESET} "{s}"')
            ts_vanished_n += 1

        # Untranslated
        for s in sorted(ts_untranslated(TS_FILE)):
            print(f'{YELLOW}UNTRANSLATED:{RESET} "{s}"')
            ts_untranslated_n += 1

    print()

    # === Summary ===
    print(f"{CYAN}=== Summary ==={RESET}")
    print(
        f"         {po_missing} missing, {po_obsolete} obsolete, "
        f"{po_untranslated} untranslated (po)"
    )
    print(
        f"         {ts_missing_n} missing, {ts_vanished_n} vanished, "
        f"{ts_untranslated_n} untranslated (ts)"
    )

    total = po_missing + po_obsolete + ts_missing_n + ts_vanished_n
    return 1 if total > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
