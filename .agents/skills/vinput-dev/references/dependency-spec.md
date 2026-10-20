# Third-Party Dependency Governance & Integration Specification

`fcitx5-vinput` enforces a strict dependency policy: **Host-First, Selective Vendoring, Zero Network during Build, and Pristine Vendoring**.

---

## 1. Hard Constraints

1. **Zero Network during Build**:
   CMake must never act as a package downloader. `FetchContent`, `git clone`, or HTTP downloads at build time are strictly prohibited. All dependencies must be provided either by host system development packages or vendored locally in the repository. Guarantees 100% deterministic builds in Nix sandboxes, Flatpak offline builders, and Arch clean chroots.
2. **Host-First Distribution (No Unbundling)**:
   Any library packaged by target Linux distributions (Ubuntu, Debian, Fedora, openSUSE, Arch, Nixpkgs) must be consumed via host packages. Never duplicate system libraries into the repository.
3. **CMake `SYSTEM INTERFACE` Isolation**:
   Vendored libraries must be declared as `SYSTEM INTERFACE` in CMake, directing the compiler and static analyzers to treat them as system headers. Exempts vendored code from `clang-tidy` diagnostics and compiler warnings without suppressing diagnostics in project code.
4. **Pristine Vendoring (Zero-Modification Iron Rule)**:
   Code under `3rdparty/` must remain strictly identical to upstream releases. Modifying, patching, editing, refactoring, or reformatting vendored code is strictly prohibited. All business glue, adapters, and compatibility wrappers must reside in `src/common/` or `src/daemon/`, never in `3rdparty/`.
5. **License Compliance Registration**:
   Every vendored library must be audited and registered in `THIRD_PARTY_NOTICES.md` with upstream URL, commit/tag, import date, and full license text.

---

## 2. Three-Tier Priority Hierarchy & Admission

Select dependency tiers sequentially from Tier 1 to Tier 3:

| Tier | Policy & Role | Scope & Examples | Build & Packaging Standard |
| :--- | :--- | :--- | :--- |
| **Tier 1: Core System Services** | Host-only; never vendor. | `fcitx5`, `pipewire`, `libsystemd`, `qt6`, `openssl` | CMake single-line `find_package(... REQUIRED)` or `pkg_check_modules`. |
| **Tier 2: Universal Modern Libraries** | Universally packaged across all target distros. | `spdlog`, `nlohmann_json`, `CLI11`, `reproc` | 1. Declare distro package in `scripts/ci-install-build-deps.sh`;<br>2. Declare package in `flake.nix` `buildInputs`;<br>3. CMake single-line `find_package(... REQUIRED)`. |
| **Tier 3: Specialized / Missing Libraries** | Missing or inconsistent across distros; lightweight header-only. | `httplib`, `base64`, `unicodelib`, `readerwriterqueue`, `mio` | Vendor under `3rdparty/`, managed via `3rdparty/CMakeLists.txt` with `SYSTEM INTERFACE`. |

### Admission Decision Tree

```text
Introduce New Dependency
       │
       ▼
Packaged across all target distributions (Debian, Ubuntu, Fedora, openSUSE, Arch, Nixpkgs)?
       │
       ├─────► YES ──────► Tier 2: Universal Modern Library
       │                   1. Declare in ci-install-build-deps.sh and flake.nix
       │                   2. CMake single-line find_package(xxx REQUIRED)
       │                   3. Never copy code into repo; never use FetchContent fallback
       │
       ▼ NO: Missing in one or more distributions
Must satisfy Tier 3 admission criteria:
1. Lightweight, focused single-header (Header-Only) or micro-library
2. Permissive open-source license (MIT, Apache-2.0, BSD)
       │
       └────────────────► Tier 3: Vendored in 3rdparty/
                          1. Store under 3rdparty/<lib_name>/
                          2. Wrap with SYSTEM INTERFACE in 3rdparty/CMakeLists.txt
                          3. Register audit in THIRD_PARTY_NOTICES.md
```

---

## 3. Tier 3 Vendoring SOP

### 3.1 Directory Layout
```text
3rdparty/
├── CMakeLists.txt
└── <lib_name>/
    ├── <lib_name>.hpp (or .h, strictly pristine upstream)
    └── LICENSE (original upstream license text)
```

### 3.2 `3rdparty/CMakeLists.txt` Isolation Pattern
Use `SYSTEM INTERFACE` to insulate vendored code:

```cmake
# Pure header-only library (e.g. base64, unicodelib)
add_library(3rdparty_<name> INTERFACE)
target_include_directories(3rdparty_<name> SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/<name>)
add_library(<name>::<name> ALIAS 3rdparty_<name>)

# Library with optional system-first detection and vendored fallback (e.g. httplib)
find_package(<name> QUIET)
if(<name>_FOUND)
    message(STATUS "Using system <name>")
else()
    message(STATUS "Using bundled <name> from 3rdparty")
    add_library(3rdparty_<name> INTERFACE)
    target_include_directories(3rdparty_<name> SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/<name>)
    add_library(<name>::<name> ALIAS 3rdparty_<name>)
endif()
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
