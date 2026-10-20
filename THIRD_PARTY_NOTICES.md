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

5. **nlohmann/json**
   - License: MIT
   - Upstream: https://github.com/nlohmann/json

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

## Models / data downloaded by users (not bundled)

1. **sherpa-onnx ASR models**
   - Notes: Model licenses vary by model and are distributed separately from this repository. Users must review the specific model license terms when downloading from sherpa-onnx releases.
   - Upstream: https://github.com/k2-fsa/sherpa-onnx (models linked from project documentation)

---

If you want full license texts included in this repository, we can add a `LICENSES/` folder and vendor the exact license files from each upstream project.
