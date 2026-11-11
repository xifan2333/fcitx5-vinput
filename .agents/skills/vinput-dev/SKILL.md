---
name: vinput-dev
description: "fcitx5-vinput 主开发路由与原则引擎 (Master development router and lifecycle engine for fcitx5-vinput). Routes intent to playbooks and enforces C++20 architecture principles (laziness protocol, C ABI boundaries, module depth, two-tier dependencies)."
---

# fcitx5-vinput Development Engine

Development routing and governance engine for `fcitx5-vinput`.

---

## 1. Intent Matching & Playbook Dispatch

Match user intent to the appropriate canonical playbook:

| Playbook | Matching Intent & Triggers | Target File |
| :--- | :--- | :--- |
| **Feature or Bug Fix** | New functionality, bug fixes, refactoring, ASR/LLM adapters, config changes. | [`playbooks/feature-or-fix.md`](playbooks/feature-or-fix.md) |
| **Audio Forensics** | PipeWire capture drops, XRuns, initial syllable clipping, daemon crashes, D-Bus timeouts. | [`playbooks/audio-forensics.md`](playbooks/audio-forensics.md) |
| **Packaging & CI** | Remote matrix validation, Dry Runs, SemVer release tagging, Actions runner management. | [`playbooks/packaging-ci.md`](playbooks/packaging-ci.md) |
| **Fork Contribution** | Contributing from external forks, remote syncing, fork hygiene. | [`playbooks/fork-contribution.md`](playbooks/fork-contribution.md) |

---

## 2. Architecture Principles (Disclosed Reference)

Consult these authoritative principles when entering the relevant domain:

- **[`principles/laziness-protocol.md`](principles/laziness-protocol.md)**: Smallest complete change. Reuse canonical `src/common/` infrastructure (`glaze`, `spdlog`, `process_utils`, `mio`, `readerwriterqueue`). Deletion beats addition. Fix root cause once in shared code.
- **[`principles/c-abi-boundaries.md`](principles/c-abi-boundaries.md)**: Six canonical C ABI boundaries (PipeWire, sd-bus, libarchive, OpenSSL, POSIX, Fcitx5 addon). Defensive wrapper guidelines.
- **[`principles/dependency-spec.md`](principles/dependency-spec.md)**: Two-tier binary boundary (Tier 1 host shared libraries vs Tier 2 `3rdparty/` header-only). Pristine 3rdparty rule. Zero network downloads during build.
- **[`principles/module-boundaries.md`](principles/module-boundaries.md)**: Strict separation of common primitives (`src/common/`) from domain orchestration (`src/cli/`, `src/daemon/`, `src/addon/`, `src/gui/`). Cognitive complexity ceiling $\le 15$.
- **[`principles/ecosystem-routes.md`](principles/ecosystem-routes.md)**: Multi-repo ecosystem routing (`vinput-registry`, `aur-auto`, `flatpak-auto`).

---

## 3. Implementation vs Review Separation

- **During Implementation**: Focus strictly on minimal, clean code conforming to `principles/laziness-protocol.md`. Do not load or overfit to review checklists during development.
- **During Review**: Audit the diff via `/vinput-review` strictly against [`CODING_STANDARDS.md`](../../CODING_STANDARDS.md).

---

## 4. Upstream Pillar Grounding (Context7)

Query Context7 before modifying the five core pillars:
- **Fcitx5 Addon (`src/addon/`)**: Context7: `/fcitx/fcitx5`.
- **Speech Engine (`src/daemon/asr/`)**: Context7: `/k2-fsa/sherpa-onnx`.
- **PipeWire Audio (`src/daemon/audio/`)**: Context7: `/pipewire/pipewire`.
- **CLI Subcommands (`src/cli/`)**: Context7: `/cliutils/cli11`.
- **Qt 6 GUI (`src/gui/`)**: Context7: `/websites/doc_qt_io_qt-6`.
