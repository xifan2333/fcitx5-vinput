#!/usr/bin/env python3
"""Validate Gettext (.po) and Qt (.ts) translations against source code via ast-grep."""

import json
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent

PO_FILE = PROJECT_ROOT / "po" / "zh_CN.po"
TS_FILE = PROJECT_ROOT / "i18n" / "vinput-gui_zh_CN.ts"

# Colors (disabled when not a terminal)
if sys.stdout.isatty():
    RED, YELLOW, CYAN, RESET = "\033[0;31m", "\033[0;33m", "\033[0;36m", "\033[0m"
else:
    RED = YELLOW = CYAN = RESET = ""


def find_ast_grep() -> str | None:
    return shutil.which("ast-grep") or shutil.which("sg")


def extract_strings_from_ast_json(stdout: str) -> set[str]:
    """Parse JSON output from ast-grep and extract concatenated string literals."""
    if not stdout.strip():
        return set()
    data = json.loads(stdout)
    res = set()
    for d in data:
        txt = d.get("text", "").strip()
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', txt)
        if parts:
            res.add("".join(p.encode().decode("unicode_escape", "ignore") for p in parts))
    return res


def extract_source_po_msgids(sg_bin: str) -> set[str]:
    """Extract Gettext _("...") and N_("...") msgids from source code via ast-grep."""
    p_ = subprocess.run(
        [sg_bin, "-p", "_($$$A)", "--json", "src/"],
        cwd=PROJECT_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    p_n = subprocess.run(
        [sg_bin, "-p", "N_($$$A)", "--json", "src/"],
        cwd=PROJECT_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return extract_strings_from_ast_json(p_.stdout) | extract_strings_from_ast_json(p_n.stdout)


def extract_source_ts_strings(sg_bin: str) -> set[str]:
    """Extract Qt tr("...") and Class::tr("...") strings from src/gui via ast-grep."""
    rule = """id: qt-tr-calls
language: Cpp
rule:
  kind: call_expression
  has:
    field: function
    regex: "(^|::)tr$"
"""
    p = subprocess.run(
        [sg_bin, "scan", "--inline-rules", rule, "src/gui/", "--json"],
        cwd=PROJECT_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return extract_strings_from_ast_json(p.stdout)


def extract_po_msgids(path: Path) -> set[str]:
    """Extract msgid strings from a .po file (skip header and obsolete #~)."""
    ids = set()
    if not path.is_file():
        return ids
    text = path.read_text(encoding="utf-8")
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


def extract_po_obsolete_msgids(path: Path) -> set[str]:
    """Extract #~ msgid strings from a .po file."""
    ids = set()
    if not path.is_file():
        return ids
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r'^#~\s*msgid "(.*)"$', line.strip())
        if m and m.group(1):
            ids.add(m.group(1).encode().decode("unicode_escape", "ignore"))
    return ids


def extract_po_untranslated(path: Path) -> set[str]:
    """Find non-obsolete msgids with empty msgstr in a .po file."""
    ids = set()
    if not path.is_file():
        return ids
    text = path.read_text(encoding="utf-8")
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
    if not path.is_file():
        return set()
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
    if not path.is_file():
        return set()
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
    """Find non-vanished entries with empty translation in a .ts file."""
    if not path.is_file():
        return set()
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


def main() -> int:
    sg_bin = find_ast_grep()
    if sg_bin is None:
        print(f"{RED}ERROR:{RESET} ast-grep (or sg) is required to run i18n checks.")
        print("       Please run `mise install` or install ast-grep.")
        return 1

    po_missing = po_obsolete = po_untranslated = 0
    ts_missing_n = ts_vanished_n = ts_untranslated_n = 0

    # === Gettext (po) ===
    print(f"{CYAN}=== Gettext (po) ==={RESET}")
    src_po_ids = extract_source_po_msgids(sg_bin)
    catalog_po_ids = extract_po_msgids(PO_FILE)

    for mid in sorted(src_po_ids - catalog_po_ids):
        print(f"{RED}MISSING:{RESET} {mid}")
        po_missing += 1

    obsolete_ids = (catalog_po_ids - src_po_ids) | extract_po_obsolete_msgids(PO_FILE)
    for mid in sorted(obsolete_ids):
        print(f"{YELLOW}OBSOLETE:{RESET} {mid}")
        po_obsolete += 1

    for mid in sorted(extract_po_untranslated(PO_FILE)):
        print(f"{YELLOW}UNTRANSLATED:{RESET} {mid}")
        po_untranslated += 1

    print()

    # === Qt (ts) ===
    print(f"{CYAN}=== Qt (ts) ==={RESET}")
    src_ts_ids = extract_source_ts_strings(sg_bin)
    catalog_ts_ids = ts_sources(TS_FILE, skip_vanished=True)

    for s in sorted(src_ts_ids - catalog_ts_ids):
        print(f'{RED}MISSING:{RESET} "{s}"')
        ts_missing_n += 1

    for s in sorted(ts_vanished(TS_FILE)):
        print(f'{YELLOW}VANISHED:{RESET} "{s}"')
        ts_vanished_n += 1

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
