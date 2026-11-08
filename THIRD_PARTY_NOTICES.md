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

8. **p-ranav/indicators** (`3rdparty/indicators/`)
   - Description: Header-only C++17 library for dynamic terminal progress bars and spinners.
   - License: MIT
   - Upstream: https://github.com/p-ranav/indicators
   - Version: v2.3 (imported 2026-10-21, vendored in `3rdparty/indicators/`)

9. **TartanLlama/expected** (`3rdparty/tl-expected/`)
    - Description: C++11/14/17/20/23 implementation of `std::expected` with monadic operations and functional extensions.
    - License: CC0-1.0 (Public Domain Dedication)
    - Upstream: https://github.com/TartanLlama/expected
    - Version: v1.3.1 (commit 1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4, imported 2026-10-22, vendored in `3rdparty/tl-expected/`)

10. **fmtlib/fmt** (`3rdparty/fmt/`)
    - Description: Fast and safe formatting library for C++.
    - License: MIT
    - License text:
      Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors

      Permission is hereby granted, free of charge, to any person obtaining
      a copy of this software and associated documentation files (the
      "Software"), to deal in the Software without restriction, including
      without limitation the rights to use, copy, modify, merge, publish,
      distribute, sublicense, and/or sell copies of the Software, and to
      permit persons to whom the Software is furnished to do so, subject to
      the following conditions:

      The above copyright notice and this permission notice shall be
      included in all copies or substantial portions of the Software.

      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
      EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
      MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
      NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
      LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
      OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
      WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

      --- Optional exception to the license ---

      As an exception, if, as a result of your compiling your source code, portions
      of this Software are embedded into a machine-executable object form of such
      source code, you may redistribute such embedded portions in such object form
      without including the above copyright and permission notices.
    - Upstream: https://github.com/fmtlib/fmt
    - Version: v11.1.4 (imported 2026-11-02, vendored in `3rdparty/fmt/`)

11. **gabime/spdlog** (`3rdparty/spdlog/`)
    - Description: Fast C++ logging library.
    - License: MIT
    - License text:
      The MIT License (MIT)

      Copyright (c) 2016 Gabi Melman.

      Permission is hereby granted, free of charge, to any person obtaining a copy
      of this software and associated documentation files (the "Software"), to deal
      in the Software without restriction, including without limitation the rights
      to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
      copies of the Software, and to permit persons to whom the Software is
      furnished to do so, subject to the following conditions:

      The above copyright notice and this permission notice shall be included in
      all copies or substantial portions of the Software.

      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
      IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
      FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
      AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
      LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
      OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
      THE SOFTWARE.
    - Upstream: https://github.com/gabime/spdlog
    - Version: v1.15.1 (imported 2026-11-03, vendored in `3rdparty/spdlog/`)

12. **CLIUtils/CLI11** (`3rdparty/cli11/`)
    - Description: Command line parser for C++11 and beyond.
    - License: BSD-3-Clause
    - License text:
      CLI11 2.2 Copyright (c) 2017-2024 University of Cincinnati, developed by Henry
      Schreiner under NSF AWARD 1414736. All rights reserved.

      Redistribution and use in source and binary forms of CLI11, with or without
      modification, are permitted provided that the following conditions are met:

      1. Redistributions of source code must retain the above copyright notice, this
         list of conditions and the following disclaimer.
      2. Redistributions in binary form must reproduce the above copyright notice,
         this list of conditions and the following disclaimer in the documentation
         and/or other materials provided with the distribution.
      3. Neither the name of the copyright holder nor the names of its contributors
         may be used to endorse or promote products derived from this software without
         specific prior written permission.

      THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
      ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
      WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
      DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
      ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
      (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
      LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
      ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
      (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
      SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
    - Upstream: https://github.com/CLIUtils/CLI11
    - Version: v2.4.2 (commit 6c7b07a878ad834957b98d0f9ce1dbe0cb204fc9, imported 2026-11-04, vendored in `3rdparty/cli11/`)

13. **yhirose/cpp-httplib** (`3rdparty/httplib/`)
    - Description: A C++11 single-file header-only cross-platform HTTP/HTTPS and WebSocket library.
    - License: MIT
    - License text:
      The MIT License (MIT)

      Copyright (c) 2017 yhirose

      Permission is hereby granted, free of charge, to any person obtaining a copy
      of this software and associated documentation files (the "Software"), to deal
      in the Software without restriction, including without limitation the rights
      to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
      copies of the Software, and to permit persons to whom the Software is
      furnished to do so, subject to the following conditions:

      The above copyright notice and this permission notice shall be included in all
      copies or substantial portions of the Software.

      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
      IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
      FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
      AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
      LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
      OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
      SOFTWARE.
    - Upstream: https://github.com/yhirose/cpp-httplib
    - Version: v0.62.0 (commit 74ef3c42188acd6391f027c4337fb0e1a68c0024, imported 2026-11-04, vendored in `3rdparty/httplib/`)

## Models / data downloaded by users (not bundled)

1. **sherpa-onnx ASR models**
   - Notes: Model licenses vary by model and are distributed separately from this repository. Users must review the specific model license terms when downloading from sherpa-onnx releases.
   - Upstream: https://github.com/k2-fsa/sherpa-onnx (models linked from project documentation)

---

If you want full license texts included in this repository, we can add a `LICENSES/` folder and vendor the exact license files from each upstream project.
