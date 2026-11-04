# Third-Party Dependency Governance & Integration Specification

`fcitx5-vinput` enforces a strict dependency policy: **Host-First, Selective Vendoring, Zero Network during Build, and Pristine Vendoring**.

---

## 1. Hard Constraints

1. **Zero Network during Build**:
   CMake must never act as a package downloader. `FetchContent`, `git clone`, or HTTP downloads at build time are strictly prohibited. All dependencies must be provided either by host system development packages or vendored locally in the repository. Guarantees 100% deterministic builds in Nix sandboxes, Flatpak offline builders, and Arch clean chroots.
2. **Host-First Distribution (No Unbundling)**:
   Any shared library (`*.so`) packaged by target Linux distributions (Ubuntu, Debian, Fedora, openSUSE, Arch, Nixpkgs) must be consumed via host packages. Never duplicate system shared libraries into the repository. Pure header-only libraries follow Section 2.
3. **CMake `SYSTEM INTERFACE` Isolation**:
   Vendored libraries must be declared as `SYSTEM INTERFACE` in CMake, directing the compiler and static analyzers to treat them as system headers. Exempts vendored code from `clang-tidy` diagnostics and compiler warnings without suppressing diagnostics in project code.
4. **Pristine Vendoring (Zero-Modification Iron Rule)**:
   Code under `3rdparty/` must remain strictly identical to upstream releases. Modifying, patching, editing, refactoring, or reformatting vendored code is strictly prohibited. All business glue, adapters, and compatibility wrappers must reside in `src/common/` or `src/daemon/`, never in `3rdparty/`.
5. **License Compliance Registration**:
   Every vendored library must be audited and registered in `THIRD_PARTY_NOTICES.md` with upstream URL, commit/tag, import date, and full license text.

---

## 2. Two-Tier Dependency Governance (Host Shared Libraries vs. Self-Contained Header-Only)

Dependencies are governed by a strict binary boundary based on **runtime ABI presence**, rather than whether a Linux distribution happens to package a header-only snapshot:

| Tier | Policy & Role | Scope & Examples | Build & Packaging Standard |
| :--- | :--- | :--- | :--- |
| **Tier 1: System ABI & Shared Libraries (`*.so`)** | Host-only; never vendor. | `fcitx5`, `pipewire`, `libsystemd`, `qt6`, `openssl`, `libarchive`, `gettext` | 1. Declare distro package in `scripts/ci-install-build-deps.sh`, `packaging/`, and `flake.nix`;<br>2. CMake single-line `find_package(... REQUIRED)` or `pkg_check_modules(... REQUIRED)`. |
| **Tier 2: Pure C++ Header-Only Libraries** | Self-contained in `3rdparty/`; never consume from host or `FetchContent`. | `fmt`, `spdlog`, `CLI11`, `httplib`, `glaze`, `tl-expected`, `mio`, `readerwriterqueue`, `subprocess`, `scope`, `tabulate`, `indicators`, `base64`, `unicodelib` | 1. Vendor pristine upstream snapshot under `3rdparty/<lib_name>/`;<br>2. Expose via `SYSTEM INTERFACE` in `CMakeLists.txt`;<br>3. Register license and version in `THIRD_PARTY_NOTICES.md`. |

### Rationale for Self-Contained Header-Only Policy

Consuming header-only C++ libraries from host distribution packages introduces severe cross-distro version skew (e.g., Debian 12 vs. Ubuntu 24.04 vs. Arch rolling), leading to C++20 `consteval` incompatibilities, missing features (such as WebSocket support in older `cpp-httplib`), and conflicting `IWYU pragma` / `clang-tidy` behavior. Self-containing all header-only libraries under `3rdparty/` guarantees:
1. **100% Deterministic C++20 Compilation & Static Analysis** across all target distributions;
2. **Zero Version Fallbacks & Zero `FetchContent`** in `CMakeLists.txt`;
3. **Minimal Packaging Surface** (`Build-Depends` / `makedepends` only track true Tier 1 shared libraries).

### Admission Decision Tree

```text
Introduce New Dependency
       │
       ▼
Does it require a separately installed runtime shared library (`.so`) or plugin ABI?
       │
       ├─────► YES ──────► Tier 1: System ABI & Shared Library
       │                   1. Declare in ci-install-build-deps.sh, packaging/, and flake.nix
       │                   2. CMake single-line find_package(xxx REQUIRED) or pkg_check_modules
       │                   3. Never copy into repo; never use FetchContent
       │
       ▼ NO: Pure C++ Header-Only Library
Must satisfy Tier 2 admission criteria:
1. Header-only library with zero compiled shared library runtime
2. Permissive open-source license (MIT, Apache-2.0, BSD, BSL-1.0, CC0)
       │
       └────────────────► Tier 2: Self-Contained in 3rdparty/
                          1. Store pristine upstream headers + LICENSE under 3rdparty/<lib_name>/
                          2. Wrap with SYSTEM INTERFACE in CMakeLists.txt (no find_package/FetchContent)
                          3. Register audit in THIRD_PARTY_NOTICES.md
```

---

## 3. Tier 2 Header-Only Vendoring SOP

### 3.1 Directory Layout
```text
3rdparty/
└── <lib_name>/
    ├── include/ or <lib_name>.hpp (strictly pristine upstream headers)
    └── LICENSE (original upstream license text)
```

### 3.2 `CMakeLists.txt` `SYSTEM INTERFACE` Isolation Pattern
Use `SYSTEM INTERFACE` unconditionally (without `find_package` or `FetchContent` fallbacks) to guarantee identical headers across all builds and insulate vendored code from project `clang-tidy` checks:

```cmake
add_library(3rdparty_<name> INTERFACE)
target_include_directories(3rdparty_<name> SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/<name>/include  # Use ${CMAKE_SOURCE_DIR}/3rdparty/<name> when headers live directly in 3rdparty/<name>/
)
add_library(<name>::<name> ALIAS 3rdparty_<name>)
```

### 3.3 License Compliance (`THIRD_PARTY_NOTICES.md`)
Every vendored library must append a notice block in `THIRD_PARTY_NOTICES.md`:
- Library name and upstream GitHub URL;
- Pinned commit hash or release tag, plus import date;
- Upstream license identifier and full license text.

---

## 4. Consumption & Wrapping Principles

1. **No Reinventing the Wheel**:
   Character classification, CJK/East Asian width calculation, Base64 encoding/decoding, and cross-platform mmap must directly consume vendored standard libraries (`unicodelib`, `base64`, `mio`). Never hand-roll lookup tables or bit-shifting state machines.
2. **No Shallow Facades (YAGNI)**:
   For localized domain use (e.g. Base64 encoding in audio transmission), include the library header directly at the call site. Do not build zero-value pass-through wrapper functions.
3. **Encapsulate Only Compound Domain Logic**:
   Abstract helper functions under `src/common/utils/` only when combining multi-library operations into a higher-order domain algorithm (e.g. grapheme cluster boundary detection + width budget allocation + ellipsis truncation).
