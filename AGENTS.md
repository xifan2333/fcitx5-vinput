# fcitx5-vinput Agent Guide

Routing index. Execution detail lives behind the pointers below.

> Language: reply in the user's conversational language.

## Module Boundaries

- `src/common/` — shared domain types, configuration (glaze), RAII wrappers, D-Bus interfaces
- `src/daemon/` — core service, PipeWire capture, ASR inference, D-Bus server
- `src/addon/` — Fcitx5 plugin, hotkeys, push-to-talk, D-Bus client
- `src/cli/` — standalone CLI
- `src/gui/` — Qt 6 configuration GUI

## Hard Invariants

1. **Bootstrap Gate** — on any development, bug-fix, or refactoring task, read
   `.agents/skills/vinput-dev/SKILL.md` before searching, editing, or branching.
2. **Issue Anchor** — every functional change traces to an existing Issue.
3. **Draft PR First** — an empty commit and an all-unchecked checklist exist before
   any functional code.
4. **Single Writer in Lockstep** — one commit per checklist item, pushed together
   with its checkbox.
5. **CI-Only Compilation** — remote CI builds. Locally run only
   `mise run check:changed`.

## Intent Routing

| Intent | Pointer |
| --- | --- |
| Develop, fix, refactor | `.agents/skills/vinput-dev/SKILL.md` |
| Review, PR gate, bot triage | `.agents/skills/vinput-review/SKILL.md` |
| Crash, dropout, audio fault | `.agents/skills/vinput-diagnose/SKILL.md` |
| Session retro, rule evolution | `.agents/skills/vinput-retro/SKILL.md` |
| Architectural judgement | `CODING_STANDARDS.md` (review phase only) |
| Domain vocabulary | `GLOSSARY.md` |
