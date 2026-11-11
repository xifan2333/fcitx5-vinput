# Module Boundaries, Complexity Budgets, and Space-Time Trade-offs

Authoritative architectural specification governing module layering, objective complexity gates, and computational resource trade-offs in `fcitx5-vinput`.

---

## 1. Architectural Boundary Mandate: Common Extraction vs. Domain Cohesion

`fcitx5-vinput` enforces a strict two-tier architectural separation between generic shared infrastructure (`src/common/`) and distinct subsystem business domains (`src/cli/`, `src/daemon/`, `src/addon/`, `src/gui/`).

```
                    ┌────────────────────────────────────────────────────────┐
                    │      src/common/ (Shared Canonical Infrastructure)     │
                    │  - StringUtils (pure algorithms, zero-copy, stateless) │
                    │  - ProcessUtils / ProcessSession (streaming pipelines) │
                    │  - FileUtils (atomic I/O, file locks, path resolution) │
                    │  - JsonAccess (Glaze reflection & typed serialization) │
                    │  - ConfigMigration (unidirectional versioned engine)   │
                    └────────────────────────────────────────────────────────┘
                                   ▲            ▲            ▲
          ┌────────────────────────┴───┐        │        ┌───┴────────────────────────┐
          │                            │        │        │                            │
┌──────────────────────┐   ┌──────────────────────┐   ┌──────────────────────┐   ┌──────────────────────┐
│       src/cli/       │   │      src/daemon/     │   │      src/addon/      │   │       src/gui/       │
│   (CLI Subsystem)    │   │  (Daemon Subsystem)  │   │   (Fcitx5 Addon)     │   │     (Qt6 Subsystem)  │
│ - Table Formatter    │   │ - Audio Capture      │   │ - Key Event Filter   │   │ - Page Controllers   │
│ - Command Dispatch   │   │ - ASR Backends       │   │ - PTT State Machine  │   │ - Item Delegates     │
│ - Resource Displays  │   │ - Postprocess Pipes  │   │ - Tray Menu UI       │   │ - Form Validation    │
└──────────────────────┘   └──────────────────────┘   └──────────────────────┘   └──────────────────────┘
```

### A. Common Infrastructure (`src/common/`) Contracts
1. **Zero Domain Awareness**: Modules in `src/common/` must remain pure primitives. They must never include headers from or depend on `cli/`, `daemon/`, `addon/`, or `gui/`.
2. **Generic Extraction Rule**: Any procedural parsing, stream slicing, escape handling, encoding transformation, or subprocess execution that can be reused across subsystems **must** be extracted into `src/common/`.
3. **Immutability and Pure Functions**: Prefer stateless functions operating on `std::string_view` or immutable inputs, returning structured domain values.

### B. Business Domain Contracts (`src/cli/`, `src/daemon/`, `src/addon/`, `src/gui/`)
1. **Orchestration Only**: Domain code is strictly reserved for high-level business workflow assembly (e.g., column width orchestration, D-Bus method dispatch, PipeWire stream lifecycle).
2. **Prohibition of Hand-Rolled Parsers (Anti-Byte-Pumping)**: Domain modules are strictly forbidden from hand-rolling character-by-character scanners, raw index loops, or escape-sequence state machines. All text sanitization and measurement must be delegated to `src/common/utils/string_utils.h`.

---

## 2. Objective Cognitive Complexity & Static Gates

Rather than relying on vague stylistic preferences, code quality is enforced by objective, mechanical compiler checks via Clang-Tidy.

### A. The Cognitive Complexity Ceiling ($\le 15$)
Every function in touched files must strictly satisfy:
$$\text{Cognitive Complexity} \le 15$$
Checked automatically via `readability-function-cognitive-complexity` in `.clang-tidy` and `mise run check:complexity`.

### B. Structural Anti-Patterns and Mandatory Modern C++20 Alternatives
1. **No Nested Loops for Parsing**:
   - *Banned*: Outer `while (!cur.empty())` with inner `while (idx < len)` scanning characters.
   - *Mandatory*: Standard C++20 algorithms (`std::ranges::find_if`, `std::string_view::find_first_of`, `std::string_view::find`).
2. **No Mutable State Flags (`bool in_flag`)**:
   - *Banned*: Setting and checking ad-hoc booleans across branching statements.
   - *Mandatory*: Chunk-hopping, declarative token slicing, or structured types.
3. **No Redundant Multi-Pass Scans**:
   - *Banned*: Scanning a string once to strip escapes, scanning again to check unclosed sequences, and scanning a third time to measure width.
   - *Mandatory*: Single-pass structured transformer (e.g., `SanitizeTerminalCell`) that produces both cleaned text and visual metrics simultaneously.

---

## 3. Space-Time Trade-offs & Allocation Budgets

In high-performance C++20 development, time and space are fungible. We explicitly favor intentional, constant-factor space trade-offs to eliminate dynamic allocations and latency regressions.

### A. Principle of Deliberate Space-Time Trade-offs
1. **Capacity Reservation (`reserve`)**:
   Always reserve string and vector capacity upfront when the upper bound is known or can be estimated (`result.reserve(text.size())`). Trading minor unused virtual memory to eliminate repeated heap reallocations (`realloc` + `memcpy`) is required.
2. **Zero-Copy Views (`std::string_view`)**:
   Pass views by value. Avoid intermediate `std::string` allocations during tokenization, slicing, or width measurement.
3. **Structured Single-Pass Results (Cache over Recompute)**:
   When transforming an input (e.g. a table cell), bundle the computed metrics (display width, sanitization state) together into an immutable struct (`ProcessedTerminalCell`). Paying for a small struct on the stack saves duplicate downstream traversals and measurements.

---

## 4. Agent Goal-Driven Solver Protocol

AI coding agents must not blindly apply rules or introduce over-engineered abstractions. Instead, treat toolchain diagnostics as objective cost functions:

1. **Observe Diagnostic**: When Clang-Tidy flags cognitive complexity or static analysis warnings, locate the branch nesting or state mutation hotspot.
2. **Identify Common vs. Domain Misplacement**: Determine if generic parsing logic was mistakenly written inside a domain module. If so, move the primitive to `src/common/`.
3. **Apply Standard Algorithms**: Replace manual pointer or index loops with standard library algorithms. Verify complexity drops below 15 and allocations remain minimal.
4. **Verify Gates**: Run `mise run check:changed`, `mise run check:complexity`, and `mise run check:dup` before finalizing changes.
