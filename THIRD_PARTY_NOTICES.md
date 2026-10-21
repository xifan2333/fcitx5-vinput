# Third-Party Notices for fcitx5-vinput

Generated: 2026-03-06

This file lists third-party software that this project depends on or bundles, and the licenses they are available under. It is a best-effort summary for compliance and attribution. For full license texts, refer to the upstream projects.

## Build/runtime dependencies (not bundled)

1. **sherpa-onnx** (speech recognition engine)
   - License: Apache-2.0
   - Upstream: https://github.com/k2-fsa/sherpa-onnx
   - Note: includes ONNX Runtime (MIT, https://github.com/microsoft/onnxruntime)

2. **Fcitx5** (input method framework)
   - License: LGPL-2.1-or-later AND Unicode-DFS-2016
   - Upstream: https://github.com/fcitx/fcitx5

2. **PipeWire** (media server / client libraries)
   - License: MIT AND LGPL-2.1-or-later AND GPL-2.0-only (project contains multiple licenses)
   - Upstream: https://pipewire.org

3. **systemd / libsystemd**
   - License: GPL-2.0 and LGPL-2.1 (project contains components under both licenses)
   - Upstream: https://github.com/systemd/systemd

4. **curl / libcurl**
   - License: curl license (MIT/X derivative, SPDX identifier: `curl`)
   - Upstream: https://curl.se/

## Bundled third-party libraries (in `3rdparty/`)

1. **cpp-subprocess** (cross-platform subprocess library for modern C++)
   - License: MIT
   - Upstream: https://github.com/arun11299/cpp-subprocess
   - Version: v2.5 (vendored in `3rdparty/subprocess/`)

2. **scope-lite** (C++ standard libraries extensions, version 3: Generic scope and RAII Wrapper)
   - License: BSL-1.0 (Boost Software License 1.0)
   - Upstream: https://github.com/martinmoene/scope-lite
   - Version: v0.3.0 (vendored in `3rdparty/scope/`)

3. **tobiaslocker/base64** (`3rdparty/base64/`)
   - Description: Fast C++17/20 header-only Base64 encode and decode library.
   - License: MIT
   - Upstream: https://github.com/tobiaslocker/base64

4. **yhirose/cpp-unicodelib** (`3rdparty/unicodelib/`)
   - Description: C++17/20 Unicode table and algorithm library supporting Unicode 18.0.0 East Asian Width (UAX #11), character categories, and grapheme clusters.
   - License: MIT
   - Upstream: https://github.com/yhirose/cpp-unicodelib

5. **cameron314/readerwriterqueue** (`3rdparty/readerwriterqueue/`)
   - Description: A fast, single-producer single-consumer lock-free queue for C++.
   - License: BSD-2-Clause (Simplified BSD License) AND Zlib (Jeff Preshing's semaphore implementation embedded in `atomicops.h`)
   - Upstream: https://github.com/cameron314/readerwriterqueue
   - Version: v1.0.7 (commit 8b21766, imported 2026-10-21, vendored in `3rdparty/readerwriterqueue/`)

6. **mandreyel/mio** (`3rdparty/mio/`)
   - Description: Cross-platform C++11 header-only memory mapped file I/O library.
   - License: MIT
   - Upstream: https://github.com/mandreyel/mio
   - Version: commit 8b6b7d8 (imported 2026-10-21, vendored in `3rdparty/mio/`)

7. **stephenberry/glaze** (`3rdparty/glaze/`)
   - Description: Extremely fast, in-memory, C++20/C++23 header-only JSON and reflection library.
   - License: MIT
   - Upstream: https://github.com/stephenberry/glaze
   - Version: v2.9.5 (commit 7c298ef6c403c505cf6291b686e32d943f1fb102, imported 2026-10-21, vendored in `3rdparty/glaze/`)

8. **p-ranav/tabulate** (`3rdparty/tabulate/`)
   - Description: Header-only C++17 library for pretty-printing tables in the terminal, with native CJK wide-character alignment and ANSI escape awareness.
   - License: MIT
   - Upstream: https://github.com/p-ranav/tabulate
   - Version: v2.2 (imported 2026-10-21, vendored in `3rdparty/tabulate/`)

9. **p-ranav/indicators** (`3rdparty/indicators/`)
   - Description: Header-only C++17 library for dynamic terminal progress bars and spinners.
   - License: MIT
   - Upstream: https://github.com/p-ranav/indicators
   - Version: v2.3 (imported 2026-10-21, vendored in `3rdparty/indicators/`)

10. **TartanLlama/expected** (`3rdparty/tl-expected/`)
    - Description: C++11/14/17/20/23 implementation of `std::expected` with monadic operations and functional extensions.
    - License: CC0-1.0 (Public Domain Dedication)
    - Upstream: https://github.com/TartanLlama/expected
    - Version: v1.3.1 (commit 1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4, imported 2026-10-22, vendored in `3rdparty/tl-expected/`)

## Models / data downloaded by users (not bundled)

1. **sherpa-onnx ASR models**
   - Notes: Model licenses vary by model and are distributed separately from this repository. Users must review the specific model license terms when downloading from sherpa-onnx releases.
   - Upstream: https://github.com/k2-fsa/sherpa-onnx (models linked from project documentation)

---

If you want full license texts included in this repository, we can add a `LICENSES/` folder and vendor the exact license files from each upstream project.
