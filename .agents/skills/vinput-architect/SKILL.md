---
name: vinput-architect
description: "Pre-implementation architecture and deep module design for fcitx5-vinput. Design deep module boundaries, C++20 data structures, zero-copy audio pipelines, and D-Bus interfaces before writing implementation code. Use when starting a new feature, planning a refactoring, or defining subsystem seams."
---

# vinput-architect

Design deep module boundaries and settle data structures, types, and seams before writing implementation code in `fcitx5-vinput`.

---

## Process

### 1. Ubiquitous Language & Domain Modeling
Consult `GLOSSARY.md` at the repository root.
- When naming domain concepts (sessions, orders, audio chunks, transitions, scene models, backend instances), use terms defined in `GLOSSARY.md`.
- Never invent synonyms that drift from canonical domain names.

### 2. Module Depth & Seam Placement (The Deletion Test)
Apply John Ousterhout's *A Philosophy of Software Design*:
- **Modules must be deep**: Extensive capability hidden behind a small, cohesive interface.
- **Reject shallow wrappers**: Do not create intermediate helper classes or one-line forwarding functions whose interface is nearly as complex as their implementation.
- **Apply the Deletion Test**: If this module were deleted or inlined, would complexity concentrate into one cohesive unit, or merely shift around? If it concentrates, the module is shallow—rethink or inline it.

### 3. Real-Time & Audio Capture Safety (PipeWire / SPSC)
If modifying or designing audio capture, playback, or callback paths:
- **Lock-free & allocation-free**: Inside `processCallback` (or any thread handling raw audio buffers), strictly forbid heap allocation (`new`, `malloc`, `make_unique`) and blocking locks (`std::mutex::lock`).
- **Buffer handoff**: Audio frames must be placed in a single-producer single-consumer queue (`readerwriterqueue`) and processed asynchronously by the daemon worker.
- **Immediate buffer release**: Return PipeWire SPA buffers immediately without format conversion or VAD computation in the callback.

### 4. Boundary Discipline (Guard the Edges, Trust the Internals)
- Validate inputs, decode JSON, and verify constraints strictly at outer system boundaries: CLI options (`CLI11`), D-Bus RPC handlers, JSON configurations (`glaze`), and PipeWire stream parameters.
- Translate external data into strongly-typed domain structs once at the edge.
- Pure functions and internal domain logic trust these invariants without scattered redundant null-checks or defensive guards.

### 5. Settle Types & Error Handling
- Use C++20 expressive types: `std::optional`, `std::variant`, and `tl::expected<T, E>`.
- Make illegal states unrepresentable.
- Subprocess execution must go through `src/common/utils/process_utils.h` (`vinput::process::Run`). Raw `system()` or `popen()` is prohibited.

### 6. Architectural Pre-Action Summary
Before writing implementation code, output a brief to the maintainer:
- Proposed interface / function signatures.
- Data structures and ownership lifecycle (RAII).
- Potential impact on memory and real-time audio threads.
