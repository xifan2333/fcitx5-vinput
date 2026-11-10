#!/usr/bin/env python3
"""Validate Gettext (.po) and Qt (.ts) translations against C++ source code."""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent

PO_FILE = PROJECT_ROOT / "po" / "zh_CN.po"
TS_FILE = PROJECT_ROOT / "i18n" / "vinput-gui_zh_CN.ts"

PO_DIRS = ["src/cli", "src/common", "src/addon", "src/daemon"]
GUI_DIR = "src/gui"

# Colors (disabled when not a terminal)
if sys.stdout.isatty():
    RED, YELLOW, CYAN, RESET = "\033[0;31m", "\033[0;33m", "\033[0;36m", "\033[0m"
else:
    RED = YELLOW = CYAN = RESET = ""

# Regex patterns matching C++ localization macros/functions and string literal concatenation
CALL_PO_RE = re.compile(
    r'(?:^|[^a-zA-Z0-9_])(?:_|\bN_)\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)', re.DOTALL
)
CALL_TS_RE = re.compile(
    r'(?:^|[^a-zA-Z0-9_])(?:([a-zA-Z0-9_:]+)::)?tr\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)(?:,[^)]*)?\)',
    re.DOTALL,
)
STR_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def extract_source_po_msgids() -> set[str]:
    """Extract Gettext _("...") and N_("...") msgids from C++ source files."""
    src_files = []
    for d in PO_DIRS:
        p = PROJECT_ROOT / d
        if p.is_dir():
            src_files.extend(sorted(str(f) for f in p.rglob("*") if f.suffix in (".cpp", ".h")))
    res = set()
    for f in src_files:
        content = Path(f).read_text(encoding="utf-8")
        for m in CALL_PO_RE.finditer(content):
            parts = STR_LITERAL_RE.findall(m.group(1))
            if parts:
                res.add("".join(p.encode().decode("unicode_escape", "ignore") for p in parts))
    return res


def extract_source_ts_strings() -> tuple[set[str], list[tuple[str, str]]]:
    """Extract Qt tr("...") and Class::tr("...") strings from src/gui/."""
    gui_files = sorted(
        str(f) for f in (PROJECT_ROOT / GUI_DIR).rglob("*") if f.suffix in (".cpp", ".h")
    )
    all_strings = set()
    qualified_calls = []
    for f in gui_files:
        content = Path(f).read_text(encoding="utf-8")
        for m in CALL_TS_RE.finditer(content):
            qualifier = m.group(1) or ""
            parts = STR_LITERAL_RE.findall(m.group(2))
            if parts:
                joined = "".join(p.encode().decode("unicode_escape", "ignore") for p in parts)
                all_strings.add(joined)
                if qualifier:
                    qualified_calls.append((qualifier, joined))
    return all_strings, qualified_calls


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


def parse_ts_catalog(path: Path) -> tuple[set[str], dict[str, set[str]], set[str], set[str]]:
    """Parse .ts file returning (all_sources, context_sources, vanished, untranslated)."""
    if not path.is_file():
        return set(), {}, set(), set()
    tree = ET.parse(path)
    all_sources = set()
    context_sources: dict[str, set[str]] = {}
    vanished = set()
    untranslated = set()

    for ctx in tree.findall("context"):
        cname = ctx.find("name").text if ctx.find("name") is not None else ""
        context_sources.setdefault(cname, set())

        for msg in ctx.findall("message"):
            tr = msg.find("translation")
            is_vanished = tr is not None and tr.get("type") in ("vanished", "obsolete")
            src = msg.find("source")
            if src is not None and src.text:
                if is_vanished:
                    vanished.add(src.text)
                else:
                    all_sources.add(src.text)
                    context_sources[cname].add(src.text)
                    if tr is None or not (tr.text or "").strip():
                        untranslated.add(src.text)

    return all_sources, context_sources, vanished, untranslated


def main() -> int:
    po_missing = po_obsolete = po_untranslated = 0
    ts_missing_n = ts_vanished_n = ts_untranslated_n = 0

    # === Gettext (po) ===
    print(f"{CYAN}=== Gettext (po) ==={RESET}")
    src_po_ids = extract_source_po_msgids()
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
    src_ts_ids, qualified_calls = extract_source_ts_strings()
    catalog_ts_ids, context_sources, vanished_ts, untranslated_ts = parse_ts_catalog(TS_FILE)

    # 1. Check flat coverage
    for s in sorted(src_ts_ids - catalog_ts_ids):
        print(f'{RED}MISSING:{RESET} "{s}"')
        ts_missing_n += 1

    # 2. Check context-qualified coverage (e.g. ClientDialog::tr("Error") must exist under ClientDialog)
    for qual, s in qualified_calls:
        matched_contexts = [c for c in context_sources if c == qual or c.endswith("::" + qual)]
        if matched_contexts and not any(s in context_sources[c] for c in matched_contexts):
            print(f'{RED}MISSING (context {qual}):{RESET} "{s}"')
            ts_missing_n += 1

    for s in sorted(vanished_ts):
        print(f'{YELLOW}VANISHED:{RESET} "{s}"')
        ts_vanished_n += 1

    for s in sorted(untranslated_ts):
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
