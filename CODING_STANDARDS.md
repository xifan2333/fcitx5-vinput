# Coding Standards

Authoritative engineering rules for code review in `fcitx5-vinput`.

> **Implementation vs. Review Notice**:
> - **Implementation Agents**: Keep context focused on minimal working diffs under `principles/laziness-protocol.md`. Do not pre-load this entire file during active coding.
> - **Review Agents (`/vinput-review`) & Human Reviewers**: Audit diffs against the judgement standards below.
> - **Mechanical Checks**: Formatting, AST patterns, cognitive complexity, and duplication are strictly enforced by automated tooling (`mise run check:changed`). Reviewers skip anything tooling already enforces.

---

## 1. Architecture & Module Depth

### Modules must be deep, not shallow
Favor deep modules: substantial functionality hidden behind a compact, intuitive interface. Avoid shallow wrappers—classes or functions whose interface is as complex as their implementation, or one-line forwarding adapters that merely pass parameters through.

- **The Cost**: Shallow modules inflate cognitive load, scatter domain logic across multiple files, and obscure real defects.
- **Review Test**: The Deletion Test—does deleting the wrapper concentrate complexity into one cohesive unit, or merely shift it?

### Seam Discipline & The Single-Implementation Rule
Never introduce an interface, abstract base class, or virtual methods solely to facilitate mocking in unit tests.
- **Rule of Two**: One implementation represents a hypothetical seam; at least two genuine production implementations justify an interface. Single-implementation interfaces are indirection debt.
- **Test Surface**: The production interface is the test surface. Callers and tests cross the same seam. Tests verify observable behavior through public interfaces, never through test-only backdoors or synthetic mock interfaces.
- **Hermetic I/O Testing**: For network or IPC interactions, use lightweight in-process loopback servers (such as ephemeral localhost socket listeners) or temporary sandboxed directories rather than mocking transport interfaces in production code.

### Boundary Discipline: Guard the edges, trust the internals
Concentrate defensive checks, type validation, and input sanitization strictly at system boundaries (CLI flags, D-Bus handlers, JSON decoders, PipeWire stream initialization). Once validated and translated into domain types, internal pure functions trust their inputs without redundant defensive null-checks or range guards.

- **The Cost**: Scattered defensive checks create dead branches, mask root causes, and clutter tests with unreachable permutations.

---

## 2. Real-Time Audio & Thread Safety

### The audio callback is lock-free and allocation-free
The PipeWire audio processing callback (`processCallback`) runs on a real-time (RT) thread. Inside this callback, never allocate memory and never acquire blocking locks. Transfer captured PCM frames exclusively via lock-free single-producer single-consumer queues (`readerwriterqueue`).

- **The Cost**: Allocations or lock contention in the RT callback cause priority inversion, buffer dropouts (XRuns), audio crackling, and desktop UI stutter.

### Return SPA buffers immediately
When reading PipeWire SPA buffers, copy the raw PCM payload into the lock-free ring buffer and release the SPA buffer immediately. Never perform format conversion, VAD calculation, or ASR inference inside the PipeWire thread.

- **The Cost**: Holding SPA buffers stalls the PipeWire graph, leading to stream termination and daemon audio starvation.

---

## 3. C-ABI Boundaries & Resource Lifetimes

### Exclusive resource ownership via RAII
Manual resource deallocation, raw owning pointers, and unmanaged file descriptors are prohibited. Encapsulate all POSIX and C API resources (PipeWire streams, sd-bus messages, libarchive handles) in custom RAII wrappers immediately at the system boundary.

- **The Cost**: Resource leaks, double-free vulnerabilities, and file descriptor exhaustion on error exit paths.

### Hard C-ABI Isolation
Keep C-ABI callback trampolines strictly separate from C++20 domain logic. C-ABI wrappers must translate raw pointers and error codes into standard C++ types (`std::span`, `std::string_view`, `tl::expected`) before passing data to internal components.

---

## 4. Types, Memory & Evolution Safety

### Make illegal states unrepresentable
Prefer C++20 expressive types: `std::optional`, `std::variant`, and `tl::expected` over nullable raw pointers with paired boolean flags or error codes. When an operation can fail, return `expected<T, Error>` instead of throwing runtime exceptions across module seams.

### Breaking config changes require ConfigMigration
When altering user-facing keys or schema types in `config.json` or `vinput.conf`, provide an explicit versioned migration step in `src/common/config/config_migration.cpp`. Never maintain ad-hoc runtime fallback aliases or backward-compatibility spaghetti in domain logic.

---

## 5. Dependencies & 3rdparty Governance

### Two-Tier Binary Boundary
- **Tier 1 (Host System Shared Libraries)**: `fcitx5`, `pipewire`, `libsystemd`, `qt6`, `openssl`, `libarchive`, `gettext` must be resolved via host package managers (`find_package REQUIRED` / `pkg_check_modules`).
- **Tier 2 (Header-Only C++ Libraries)**: Pure C++ dependencies (`glaze`, `spdlog`, `fmt`, `CLI11`, `httplib`, `mio`, `readerwriterqueue`, `subprocess`, `unicodelib`) must be vendored under `3rdparty/` as CMake `SYSTEM INTERFACE` targets. Build-time network downloading (`FetchContent`) is prohibited.

### Pristine 3rdparty
Code under `3rdparty/` must remain untouched upstream snapshots. Never patch, reformat, or modify vendored code directly. All adapters, shims, and workarounds must live inside `src/`.

---

## 6. Fowler Code Smell Baseline

During review, evaluate the diff against these baseline heuristics:

- **Mysterious Name**: Name does not reveal what the entity does or holds. Rename it; if no honest name fits, the design is muddy.
- **Duplicated Code**: Identical logic structure appears in more than one place. Extract the common structure.
- **Feature Envy**: A function or method accesses data of another object more than its own. Move the logic closer to the data.
- **Data Clumps**: The same group of variables or parameters travel together repeatedly. Encapsulate them into a domain type.
- **Primitive Obsession**: Raw primitives or strings stand in for domain concepts that deserve a dedicated type.
- **Repeated Switches**: The same conditional cascade recurs across multiple call sites. Replace with polymorphism or a lookup table.
- **Shotgun Surgery**: A single logical change forces scattered modifications across unrelated files. Consolidate cohesive logic.
- **Divergent Change**: A single module is repeatedly edited for unrelated business reasons. Split by responsibility.
- **Speculative Generality**: Abstractions, hooks, or parameters added for hypothetical future needs. Delete them.
- **Middle Man**: A class or function merely delegates calls without adding meaningful logic. Collapse it.

---

## Appendix: Automated Quality Gates

The following mechanical rules are strictly audited by local tooling and CI. Reviewers skip manual verification of these items:

| Rule Category | Mechanical Enforcement | Command |
| :--- | :--- | :--- |
| **AST Anti-Patterns** | `ast-grep` (banned malloc in RT callback, banned daemon stdout, no diagnostic suppressions) | `mise run check:ast` |
| **Complexity Ceiling** | `lizard` ($\text{CCN} \le 15, \text{CogC} \le 15$, nesting $< 4$) | `mise run check:complexity` |
| **Code Duplication** | `jscpd` across `src/` | `mise run check:dup` |
| **Code Formatting** | `hk` (`clang-format`, `cmake-format`, `ruff`, `shfmt`) | `mise run fix` / `check:cmake` |
| **Config Schema** | `scripts/check-config-migration.py` | `mise run check:migration` |
| **Typos & Spelling** | `typos` | `mise run check:typos` |
