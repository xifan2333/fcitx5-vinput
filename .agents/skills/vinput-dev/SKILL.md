---
name: vinput-dev
description: "Master development router and lifecycle engine for fcitx5-vinput. Automatically routes user intent to domain playbooks (feature/fix, audio forensics, packaging CI, fork contribution) and enforces C++20 architecture principles (laziness protocol, C ABI boundaries, module depth, two-tier dependencies). Use when developing, fixing, refactoring, debugging PipeWire, running CI, or contributing to fcitx5-vinput."
---

# fcitx5-vinput Development Engine

Semi-automatic ("手自一体") development and governance engine for `fcitx5-vinput`.

---

## 1. Intent Matching & Playbook Dispatch

When receiving a task, match the user's intent to one of the 4 canonical playbooks below.

> **Execution Contract (Todo List Injection)**:
> Open a task checklist whose first items are the matched playbook's steps, copied in verbatim, before any task-specific items. Execute steps in order; never skip a step without an explicit reason.

| Playbook | Matching Intent & Triggers | Target File |
| :--- | :--- | :--- |
| **Feature or Bug Fix** | New functionality, bug fixes, refactoring, ASR/LLM adapters, config changes. | [`playbooks/feature-or-fix.md`](playbooks/feature-or-fix.md) |
| **Audio Forensics** | PipeWire capture drops (XRuns), swallowing initial syllables, daemon crashes, D-Bus failures. | [`playbooks/audio-forensics.md`](playbooks/audio-forensics.md) |
| **Packaging & CI** | Remote matrix validation, Dry Runs, SemVer release tagging, Actions runner management. | [`playbooks/packaging-ci.md`](playbooks/packaging-ci.md) |
| **Fork Contribution** | Contributing from an external fork, remote syncing, PR hygiene. | [`playbooks/fork-contribution.md`](playbooks/fork-contribution.md) |

---

## 2. Architecture Principles (Disclosed Reference)

Consult these authoritative principles whenever entering the relevant domain:

- **[`principles/laziness-protocol.md`](principles/laziness-protocol.md)**: The smallest complete change. Reuse canonical `src/common/` infrastructure (`glaze`, `spdlog`, `process_utils`, `mio`, `readerwriterqueue`). Deletion beats addition. Fix root cause once in shared code.
- **[`principles/c-abi-boundaries.md`](principles/c-abi-boundaries.md)**: 6 canonical C ABI boundaries (PipeWire, sd-bus, libarchive, OpenSSL, POSIX, Fcitx5 addon). Immunization rules against automated review bot false-positives.
- **[`principles/dependency-spec.md`](principles/dependency-spec.md)**: Two-tier binary boundary (Tier 1 host `.so` vs Tier 2 `3rdparty/` header-only). Pristine 3rdparty (zero edits). Zero network downloads during build (`FetchContent` prohibited).
- **[`principles/module-boundaries.md`](principles/module-boundaries.md)**: Strict separation of common primitives (`src/common/`) from domain orchestration (`src/cli/`, `src/daemon/`, `src/addon/`, `src/gui/`). Cognitive complexity $\le 15$.
- **[`principles/ecosystem-routes.md`](principles/ecosystem-routes.md)**: Multi-repo ecosystem routing: `vinput-registry` (zero pip deps), `aur-auto`, and `flatpak-auto`.

---

## 3. Review SSOT & Code Standards

- During implementation: Focus on minimal, working code under `principles/laziness-protocol.md`.
- Before marking PR ready / during review: Audit diff strictly against [`CODING_STANDARDS.md`](../../CODING_STANDARDS.md) at the repository root.

---

## 4. Upstream Pillar Grounding (Context7)

Before modifying the five core pillars, query Context7 to anchor canonical APIs:
- **Fcitx5 Addon (`src/addon/`)**: Context7: `/fcitx/fcitx5`.
- **Speech Engine (`src/daemon/asr/`)**: Context7: `/k2-fsa/sherpa-onnx`.
- **PipeWire Audio (`src/daemon/audio/`)**: Context7: `/pipewire/pipewire`.
- **CLI Subcommands (`src/cli/`)**: Context7: `/cliutils/cli11`.
- **Qt 6 GUI (`src/gui/`)**: Context7: `/websites/doc_qt_io_qt-6`.
