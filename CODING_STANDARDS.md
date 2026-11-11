# Coding Standards

The authoritative engineering rules an agent or human contributor holds while writing and reviewing code in `fcitx5-vinput`.

> **Implementation vs. Review Notice**:
> - **Implementation Agents**: Keep your context focused on requirements, exploration, and minimal working code.
> - **Reviewer Agents & Human Reviewers**: Load this file to rigorously audit diffs along the **Standards Axis**.
> - **Mechanical Rules** are enforced by deterministic tools (`ast-grep`, `lizard`, `jscpd`, `clang-tidy`, `hk`).
> - **Judgement Calls** require architectural evaluation against the principles below.

---

## 1. Real-Time Audio & PipeWire (实时音频与线程安全)

### The audio callback is lock-free and allocation-free
The PipeWire audio processing callback (`processCallback`) runs on a real-time (RT) thread. Inside this callback, never allocate memory (`malloc`, `new`, `std::make_unique`, `std::vector::push_back`) and never acquire blocking locks (`std::mutex::lock`). Transfer captured PCM frames exclusively via lock-free single-producer single-consumer queues (`readerwriterqueue`).

- **The Cost**: Memory allocations or lock contention in the RT callback cause priority inversion, pipewire buffer dropouts (XRuns), audio crackling, and desktop UI stutter.
- **Enforcement**: Mechanically enforced by `ast-grep` (`.ast-grep/rules/no-alloc-in-audio-callback.yml`) and CI ASan thread checks.

### Return SPA buffers immediately
When reading PipeWire SPA buffers, copy the raw PCM payload into the lock-free ring buffer and release the SPA buffer immediately. Never perform format conversion, VAD calculation, or ASR inference inside the PipeWire thread.

- **The Cost**: Holding SPA buffers stalls the PipeWire graph, leading to stream termination and daemon audio starvation.
- **Enforcement**: Reviewer judgement call (`vinput-diagnose` for tracing).

---

## 2. Architecture & Module Depth (架构与深模块设计)

### Modules must be deep, not shallow
Favor deep modules: substantial functionality hidden behind a compact, intuitive interface (Ousterhout's *A Philosophy of Software Design*). Avoid "shallow wrappers"—classes or functions whose interface is as complex as their implementation, or one-line forwarding adapters that just pass parameters through.

- **The Cost**: Shallow modules inflate cognitive load, scattering business logic across dozens of files, obscuring real bugs, and making AI agents hallucinate call chains.
- **Enforcement**: Reviewer judgement call; apply the *Deletion Test* (does deleting the module concentrate complexity into one cohesive unit, or merely shift it?).

### SSOT Infrastructure Mandate (Zero Ad-hoc Utils)
Never re-implement generic utility functions (e.g. ad-hoc string split/trim, raw `popen`/`system`, hand-rolled JSON parsers, custom base64). Always reuse canonical infrastructure in `src/common/`:
- **JSON Serialization & Reflection**: `glaze` (`#include <glaze/glaze.hpp>`)
- **Subprocess Execution**: `src/common/utils/process_utils.h` (`subprocess.hpp`)
- **Logging & Tracing**: `VLOG_INFO`, `VLOG_DEBUG`, `VLOG_ERROR` (`spdlog` wrappers)
- **Memory-Mapped Files**: `mio`
- **Lock-Free Buffers**: `readerwriterqueue`
- **CLI Parsing**: `CLI11`

- **The Cost**: Ad-hoc utilities introduce inconsistent error semantics, signal-handling vulnerabilities, buffer overflows, and maintenance fragmentation.
- **Enforcement**: Mechanically enforced by `jscpd` (`check:dup`), `ast-grep` (`anti-ad-hoc-utils`), and Reviewer audit.

### Boundary Discipline: Guard the edges, trust the internals
Concentrate defensive checks, type validation, and input sanitization strictly at system boundaries (CLI flags, D-Bus RPC handlers, JSON config decoders, PipeWire stream setup). Once validated and translated into domain types, pure functions and internal modules trust their arguments without redundant nested defensive guards.

- **The Cost**: Scattered defensive checks create dead branches, mask root causes, degrade runtime performance, and clutter tests with untestable permutations.
- **Enforcement**: Reviewer judgement call (pstack `principle-boundary-discipline`).

---

## 3. Control Flow & Cognitive Complexity (控制流与复杂度)

### Functions stay simple ($\text{CCN} \le 15, \text{CogC} \le 15$)
No function may exceed Cyclomatic Complexity (CCN) of 15 or Cognitive Complexity (CogC) of 15. If a function approaches this limit, decompose it into private helper functions, lookup tables, or composable standard algorithms (`std::ranges`).

- **The Cost**: High-complexity functions hide critical edge-case defects, increase bug density exponentially, and cause AI code generators to fail.
- **Enforcement**: Mechanically enforced by `lizard` (`scripts/check-complexity.sh`, `mise run check:complexity`).

### Flatten branching into early guard clauses
Prefer early return guard clauses over nested `if-else` blocks. Maximum nesting depth of conditional statements and loops must be $< 4$. Eliminate nested loops across dynamic collections; use hash lookups or indexed sets instead.

- **The Cost**: The "arrow anti-pattern" forces developers and agents to maintain exponential state branches in memory, leading to off-by-one errors and missed edge cases.
- **Enforcement**: Mechanically enforced by `ast-grep` (`deep-if-branching.yml`, `nested-loops.yml`).

---

## 4. Types, Memory & Lifetime Safety (类型、内存与生命周期)

### Make illegal states unrepresentable
Prefer C++20 expressive types: `std::optional`, `std::variant`, and `tl::expected` / `std::expected` over nullable raw pointers with paired boolean flags or error codes. When an operation can fail, return `expected<T, Error>` instead of throwing runtime exceptions across module seams.

- **The Cost**: Boolean flag out-of-sync bugs, null pointer dereferences, and silent error swallowing.
- **Enforcement**: Clang-Tidy + Reviewer audit.

### Exclusive resource ownership via RAII
Manual `delete`, raw owning pointers, and unmanaged file descriptors are forbidden. Encapsulate all POSIX and C API resources (PipeWire streams, sd-bus messages, libarchive handles) in custom RAII wrappers or smart pointers with custom deleters.

- **The Cost**: Memory leaks, double-free vulnerabilities, and file descriptor exhaustion on unexpected exit paths.
- **Enforcement**: Clang-Tidy (`bugprone-*`, `cppcoreguidelines-*`) + CI ASan/UBSan suites.

### Zero-Tolerance on diagnostic suppressions
Never suppress warnings or errors using `// NOLINT`, `#pragma GCC/clang diagnostic ignored`, or `-Wno-*` compiler flags in CMakeLists.txt. Address the underlying root cause by refactoring types or importing required standard headers.

- **The Cost**: Suppressions hide real compiler warnings, mask undefined behavior, and break portability across compilers (GCC vs Clang).
- **Enforcement**: Mechanically enforced by `ast-grep` (`no-diagnostic-suppressions.yml`) and CMake `-DVINPUT_ENABLE_WERROR=ON`.

---

## 5. Logging, Output & Localization (日志、输出与国际化)

### Zero stdout/stderr pollution in daemon & addon
`std::cout`, `std::cerr`, `printf`, and `puts` are strictly prohibited in `src/daemon/` and `src/addon/`. All operational diagnostics and error tracing must go through structured logging macros (`VLOG_INFO`, `VLOG_WARN`, `VLOG_ERROR`, `VLOG_DEBUG`).

- **The Cost**: Writing to stdout in the addon corrupts Fcitx5 IPC streams; unformatted stderr writes in the daemon break systemd journal rate-limiting and unit monitoring.
- **Enforcement**: Mechanically enforced by `ast-grep` (`anti-daemon-stdout.yml`).

### All user-visible strings must be localized
Any string presented to the user (GUI labels, system notifications, CLI status messages) must be wrapped in `_("...")` or `ki18n`. Never hardcode raw user-facing English strings in UI or CLI outputs.

- **The Cost**: Incomplete translation catalogs, broken multilingual desktop experience for non-English users.
- **Enforcement**: Mechanically enforced by `ast-grep` (`no-hardcoded-ui-strings.yml`) and `scripts/check-i18n.py`.

---

## 6. Configuration & Versioning (配置演进与架构兼容)

### Breaking config changes require ConfigMigration in the same PR
If altering user-facing keys or schema types in `config.json` or `vinput.conf`, add a versioned migration step in `src/common/config/config_migration.cpp` (`RegisteredSteps`). Never maintain ad-hoc runtime fallback aliases or backward-compatibility spaghetti in domain code.

- **The Cost**: Broken user configurations upon upgrade, silent configuration loss, and creeping legacy technical debt.
- **Enforcement**: Enforced by pre-commit migration audit script (`scripts/check-config-migration.py`) and Reviewer audit.

---

## 7. Dependencies & 3rdparty Governance (依赖治理与两级边界)

### Two-Tier Binary Boundary
- **Tier 1 (Host System Shared Libraries)**: `fcitx5`, `pipewire`, `libsystemd`, `qt6`, `openssl`, `libarchive`, `gettext` must be resolved via host package managers (`find_package REQUIRED` / `pkg_check_modules`).
- **Tier 2 (Header-Only C++ Libraries)**: Pure C++ dependencies (`glaze`, `spdlog`, `fmt`, `CLI11`, `httplib`, `mio`, `readerwriterqueue`, `subprocess`, `indicators`, `base64`, `unicodelib`) must be vendored under `3rdparty/` with CMake `SYSTEM INTERFACE` targets and registered in `THIRD_PARTY_NOTICES.md`. Build-time downloading (`FetchContent`) is strictly prohibited.

- **The Cost**: Cross-distro header version skew, build failures behind firewalls or in offline chroots (Nix/OBS/Arch build systems).
- **Enforcement**: CMake configuration checks + CI offline verification.

### Pristine 3rdparty (零篡改铁律)
Code under `3rdparty/` must remain untouched upstream snapshots. Never patch, reformat, or modify vendored code. All adapters, shims, and workarounds must live inside `src/`.

- **The Cost**: Merge conflicts when upgrading vendored libraries, lost upstream fixes, and untracked divergence.
- **Enforcement**: Mechanically enforced by `hk.pkl` exclusions and git diff checks.
