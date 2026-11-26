# C ABI Hard Boundary Specification & Triage Guide

This document defines the architectural boundaries between **C ABI interfaces** and **pure C++20 business logic** in `fcitx5-vinput`. It serves as the single source of truth (SSOT) for code reviews, refactoring limits, and automated review bot triage (Issue #457).

---

## 1. Architectural Philosophy: C at the Boundary, C++20 Everywhere Else

- **Core Axiom**: *Interface in C (forced by binary/kernel ABI), implementation in C++20 (mandatory wherever permitted).*
- **Hard Red Line**: Never refactor, replace, or wrap designated C ABI boundaries with modern C++ idioms (e.g. `std::function`, virtual templates, exceptions) if doing so violates C linkage, async-signal safety, or kernel/library ABIs.
- **Review Bot Immunization**: Review bots (CodeRabbit, Qodo, Greptile) frequently emit false-positive suggestions urging modernization of raw pointers, `void* userdata`, or C-style arrays at these boundaries. Such suggestions **MUST be explicitly rejected** during review triage.

---

## 2. The Six Canonical C ABI Hard Boundaries

The following six subsystems are strictly bounded by third-party C libraries or Linux kernel ABIs:

### Boundary 1: PulseAudio (`libpulse`) Audio Capture & Control
- **Files**: `src/daemon/audio/audio_capture.{h,cpp}`, `src/daemon/audio/output_ducker.{h,cpp}`, `src/common/audio/pulse_device.{h,cpp}`, `src/common/audio/pulse_raii.h`
- **ABI Constraints**:
  - **C Callbacks & Trampolines**: Callback hooks (e.g., `OnStreamRead`, `OnStreamState`, `OnContextState`, `OnSourceInfo`, `OnSinkInfo`) must be static/free C function pointers with `void* userdata` trampolines. They **must never throw C++ exceptions** across the `libpulse` C boundary.
  - **Handle Lifecycle**: `pa_mainloop*`, `pa_threaded_mainloop*`, `pa_context*`, `pa_stream*`, `pa_operation*`, and `pa_time_event*` handles must be encapsulated in RAII wrappers (`vinput::pulse::Scoped*`).
  - **Action Rule**: Retain `libpulse` C callback signatures and POD structs (`pa_sample_spec`, `pa_buffer_attr`, `pa_cvolume`) at the boundary. Internal helper logic outside callbacks must use pure C++20.

### Boundary 2: sd-bus D-Bus Integration
- **Files**: `src/daemon/runtime/dbus_service.{h,cpp}`, `src/cli/runtime/dbus_client.cpp`, `src/common/systemd/systemd_client.cpp`
- **ABI Constraints**:
  - **Vtable Definitions**: `static const sd_bus_vtable vtable[]` is a C struct array processed directly by `sd_bus_add_object_vtable`.
  - **Method Handlers**: Handlers must adhere to `int(*)(sd_bus_message*, void*, sd_bus_error*)`.
  - **Variadic Message Parsing**: Calls like `sd_bus_message_read(m, "s", &str)` rely on C format-specifier semantics.
  - **Action Rule**: Retain sd-bus C signatures intact. Delegate immediately into strongly-typed C++ member functions.

### Boundary 3: libarchive Model Package Decompression
- **Files**: `src/common/registry/registry_models.cpp`
- **ABI Constraints**:
  - **Handle Lifecycle**: Interacts with `struct archive*` and `struct archive_entry*` via `archive_read_new()`, `archive_read_open_filename()`, `archive_write_disk_new()`, etc.
  - **Error Codes**: Uses integer return codes (`ARCHIVE_OK`, `ARCHIVE_EOF`, `ARCHIVE_WARN`).
  - **Action Rule**: Retain libarchive C API calls. Wrap handles in RAII deleters where appropriate.

### Boundary 4: libcurl HTTP Client
- **Files**: `src/common/net/http_client.cpp`
- **ABI Constraints**:
  - **C Callbacks & Trampolines**: Transfer callbacks (`ReadRequestBodyCallback`, `MemoryWriteCallback`, `FileWriteCallback`, `HeaderWriteCallback`, `TransferInfoCallback`) must be C-linkage-compatible function pointers with `void* userdata` trampolines and must not let C++ exceptions escape into libcurl.
  - **Handle Lifecycle**: `CURL*` easy handles and `curl_slist*` header chains must be wrapped in RAII deleters (`std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>` and `CurlHeaderList`).
  - **Action Rule**: Retain libcurl C `setopt`/`getinfo` and callback signatures at the boundary; keep pooling, header construction, and response mapping in pure C++20.

### Boundary 5: POSIX Signals & Main Event Loop
- **Files**: `src/daemon/main.cpp`, `src/cli/control/daemon_actions.cpp`
- **ABI Constraints**:
  - **Signal Handlers**: `signal(SIGTERM, ...)` and `signal(SIGINT, ...)` handlers must be async-signal-safe functions with explicit `"C"` language linkage and internal linkage (`namespace { extern "C" { static void Handler(int) noexcept { ... } } }`), writing only to lock-free/async-signal-safe state (e.g. `std::atomic<bool>` or `eventfd`). Passing lambdas to `std::signal` / `signal` is strictly forbidden (mechanically enforced by `signal-handler-no-lambda`). For ignoring signals (such as `SIGPIPE`), use the standard POSIX macro `SIG_IGN`.
  - **Event Loop `poll()`**: `struct pollfd fds[3]` and `poll(fds, ...)` interact directly with the Linux kernel multiplexing syscall.
  - **POSIX Headers**: Include POSIX/C system headers directly (`#include <signal.h>`, `#include <sys/poll.h>`, `#include <stdlib.h>`, `#include <time.h>`, `#include <locale.h>`, `#include <systemd/sd-bus.h>`) without wrapping `#include` directives in `extern "C" { ... }`. System headers already provide C linkage guards, and `.clang-tidy` disables `modernize-deprecated-headers` while retaining `misc-include-cleaner`.
  - **Action Rule**: Retain `signal()` and `poll()` arrays unchanged. Main argument parsing (`argv`) must use `std::span` and `std::string_view` (modernized in PR #496).

### Boundary 6: Fcitx5 dlopen Addon Factory
- **Files**: `src/addon/core/vinput.cpp`
- **ABI Constraints**:
  - **Plugin Export**: `FCITX_ADDON_FACTORY(VinputEngineFactory)` expands to an `extern "C"` dynamic library export symbol used by `fcitx5` via `dlopen`/`dlsym`.
  - **Action Rule**: The factory declaration must remain untouched. The instantiated `VinputEngine` class is pure C++20.

---

## 3. Pure C++ Domain (Modernization Targets)

Any code **outside** the above six boundaries is pure C++ domain and must strictly adhere to modern C++20 conventions:

| Pattern | Prohibited C Idiom | Mandatory C++20 Idiom |
| :--- | :--- | :--- |
| **Logging** | Raw `fprintf(stderr, ...)`, `printf()` | Internal `spdlog` macros (`VLOG_INFO`, `VLOG_ERROR`, `VLOG_DEBUG`) |
| **String Ops** | `strcmp`, `strncmp`, `strlen` | `std::string_view` (`==`, `starts_with`, `substr`) |
| **Memory Ops** | Ad-hoc `char buf[]` + `memcpy` for strings | `std::string`, `std::vector`, `std::span` |
| **C Arrays** | `const char* vars[]` | `constexpr std::array<const char*, N>` |
| **Algorithms** | `std::find(c.begin(), c.end(), x)` | `std::ranges::find(c, x)` |
| **Type Casts** | `(int)val` | `static_cast<int>(val)` |
| **Type Tags** | Redundant `struct Foo var;` in C++ | `Foo var;` |
| **Env Access** | Inconsistent `getenv` checks | Canonical `GetEnvNonEmpty` or `std::string` |

---

## 4. Review Bot Triage Decision Matrix

When automated review bots (CodeRabbit, Qodo, etc.) flag code at C ABI boundaries:

1. **Verify Location**: Is the flagged line located inside one of the 6 canonical C ABI boundaries?
2. **If YES**:
   - Classify as **False Positive (Domain ABI Constraint)**.
   - Reject the recommendation in the chat triage report.
   - Do NOT apply code changes or suppression pragmas (`// NOLINT`).
3. **If NO (Pure C++ Domain)**:
   - Evaluate against C++20 standard and project guidelines.
   - If valid, accept and refactor to canonical C++20.
