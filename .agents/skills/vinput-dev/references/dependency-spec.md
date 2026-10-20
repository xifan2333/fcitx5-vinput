# fcitx5-vinput 第三方依赖管理与现代化治理规范

**文档状态**：正式架构规范 / 优秀开源对齐标准
**关联议题**：[#407 (RFC: 全面基础设施现代化方案)](https://github.com/xifan2333/fcitx5-vinput/issues/407) 及系列子 Issue (#410~#420)
**适用范围**：`fcitx5-vinput` 核心代码库、`flake.nix`、CMake 构建系统与多发行版打包流

---

## 1. 背景与核心痛点 (Context & Problem Statement)

在 `fcitx5-vinput` 推进 C++20 现代化的过程中，手写底层胶水代码与外来依赖管理暴露了三类深层次问题：

1. **手写基础设施的维护黑洞**：早期手写的大量基础设施代码（如 Unicode 东亚字符宽度区间表、Base64 查表、变参 `snprintf` 格式化、Socket/WS 协议等）随着业务复杂度提升，维护成本极高且极易出现字符劈裂、内存违规等失控隐患。
2. **CMake 充当下载器导致构建臃肿与沙箱崩溃**：过去在 `CMakeLists.txt` 中堆砌大量 `option(VINPUT_FETCH_*)` + `FetchContent(GIT_REPOSITORY ...)` 模版代码，使构建脚本极度杂乱；更严重的是，NixOS 沙箱、Flatpak 离线构建环境、Arch clean chroot 均在网络隔离（`unshare -n`）环境下编译，构建期尝试 `git clone` 直接导致构建崩溃。
3. **第三方代码风格污染项目代码体检**：外部引入的 C/C++ 库如果直接作为项目普通源码编译，其内部的老旧语法会触发项目的严格静态检查（`clang-tidy`），破坏 Hard Constraint 10（零容忍压制告警）。

---

## 2. 标杆对齐：优秀开源项目的四大治理铁律

向业界高标准开源项目（Fcitx5 官方体系、KDE、OBS Studio、DuckDB）看齐，确立四大依赖治理铁律：

1. **构建系统绝对不充当下载器（Zero Network during Build）**：
   - 编译期严禁执行任何网络拉取。所有源码要么由系统的 dev 包提供，要么自包含在源码发布 Tarball 中，满足全矩阵离线构建要求。
2. **发行版通用包“非必要不内嵌”（No Unbundling Rule）**：
   - 严格遵循 Linux 发行版打包规范。只要所有目标发行版官方均已打包的库，坚决不内嵌到仓库中，强制走系统包分发，保持仓库轻量。
3. **生态缺失的专有库统一收敛至 `3rdparty/`**：
   - 当主流发行版生态缺失、且为项目必需的轻量/单头文件专用库时，统一内嵌至本地 `3rdparty/`，源码随 Git 仓库与 Release Tarball 一并分发，保证 100% 离线确定性。
4. **CMake `SYSTEM INTERFACE` 强制隔离**：
   - 所有内嵌库必须在 CMake 中以 `SYSTEM INTERFACE` 声明，通知编译器和静态分析器将其视为系统头文件，天然豁免 `clang-tidy` 诊断与编译告警，零修改外部代码。

---

## 3. 核心架构：三层优先级阶梯决策模型 (Three-Tier Priority Hierarchy)

依据库的成熟度、体量以及目标发行版（Ubuntu 24.04, Debian 12, Fedora 43, openSUSE Leap 15.6, Arch Linux, Nixpkgs）的官方打包覆盖率，建立严格的优先级判定阶梯：

```text
                     fcitx5-vinput 现代依赖金字塔
    ┌─────────────────────────────────────────────────────────────┐
    │ Tier 1: 宿主系统核心服务 (System-Only)                       │
    │   - 代表: fcitx5, pipewire, libsystemd, qt6, openssl        │
    │   - 策略: 宿主系统包管理器提供，CMake find_package REQUIRED     │
    ├─────────────────────────────────────────────────────────────┤
    │ Tier 2: 全发行版通用现代库 (System-Distributed)              │
    │   - 代表: spdlog, nlohmann_json, CLI11, reproc              │
    │   - 策略: ci-deps 脚本 & flake.nix 声明，CMake 单行 REQUIRED  │
    │   - 净化: 彻底拔除 CMake 中所有冗余 FetchContent 兜底        │
    ├─────────────────────────────────────────────────────────────┤
    │ Tier 3: 生态缺失 / 专用 Header-Only (Vendored 3rdparty)     │
    │   - 代表: httplib, base64.hpp, unicodelib.h, readerwriterqueue │
    │   - 策略: 源码内嵌于 3rdparty/，3rdparty/CMakeLists.txt 统管 │
    │   - 特性: SYSTEM INTERFACE 隔离免报，绝对离线自包含          │
    └─────────────────────────────────────────────────────────────┘
```

### 3.1 依赖准入决策流程 (Decision Tree)

```text
引入新第三方依赖
       │
       ▼
【优先级 1：系统级分发优先 (System-First)】
检查所有目标发行版（Nixpkgs / Debian / Ubuntu / Fedora / openSUSE / Arch）：
所有发行版都有官方稳定打包吗？
       │
       ├─────► 【是】──────► 归入 Tier 2（系统级分发）
       │                     - ci-install-build-deps.sh 与 flake.nix 声明 package
       │                     - CMake 仅声明单行 find_package(xxx REQUIRED)
       │                     - 严禁复制代码进仓库，严禁在 CMake 中写 Fetch 兜底
       │
       ▼ 【否：任一主流发行版缺失官方包（如 Debian/Ubuntu 缺源、或纯 header 库无包）】
【优先级 2：本地仓库内嵌兜底 (Vendored Fallback)】
为了满足 Nix 沙箱 / Flatpak 离线构建（绝对禁止编译期联网拉取）：
只能且必须内嵌至本地仓库 `3rdparty/`
       │
       └──────────────────► - 归入 Tier 3（本地内嵌）
                            - 必须是轻量 / 单头文件库
                            - 由 3rdparty/CMakeLists.txt 统一声明 SYSTEM INTERFACE
                            - THIRD_PARTY_NOTICES.md 登记许可证与版本
```

---

## 4. #407 全量现代化候选库优先级分布清单

| 基础设施模块 | 推荐库 | 主流发行版支持度 (Nixpkgs / Debian / Arch / Fedora / openSUSE) | 归属梯队 | 接入方式与构建规范 |
| :--- | :--- | :--- | :--- | :--- |
| **网络通信** (#410) | `cpp-httplib` | ⚠️ Nix/Arch/Fedora 有，Debian/Ubuntu 官方 apt 缺失 | **Tier 3** | 内嵌至 `3rdparty/httplib`，支持系统优先探测优雅回退 |
| **结构化日志** (#411) | `spdlog` | ✅ 全矩阵系统官方预装 | **Tier 2** | `ci-deps` 声明，CMake `find_package(spdlog REQUIRED)`，消费 `spdlog::spdlog_header_only` |
| **命令行解析** | `CLI11` | ✅ 全矩阵系统官方预装 | **Tier 2** | `ci-deps` 声明，CMake `find_package(CLI11 REQUIRED)` |
| **JSON 序列化** | `nlohmann_json` | ✅ 全矩阵系统官方预装 | **Tier 2** | `ci-deps` 声明，CMake `find_package(nlohmann_json 3.2.0 REQUIRED)` |
| **子进程管理** (#417) | `reproc` | ✅ 全矩阵系统官方预装 | **Tier 2** | `ci-deps` 声明，CMake `find_package(reproc REQUIRED)` |
| **错误处理单子** (#414)| `tl-expected` | ✅ 全矩阵系统官方预装 | **Tier 2** | `ci-deps` 声明，CMake `find_package(tl-expected REQUIRED)` |
| **Base64 编解码** (#416)| `tobiaslocker/base64` | ❌ 各发行版均无单独包 | **Tier 3** | 内嵌为 `3rdparty/base64/base64.hpp` (单头文件, ~300行) |
| **Unicode 字宽/CJK** (#416)| `yhirose/cpp-unicodelib` | ❌ 各发行版无单独包 | **Tier 3** | 内嵌为 `3rdparty/unicodelib/unicodelib.h` (单头文件) |
| **音频无锁队列** (#413)| `readerwriterqueue` | ⚠️ Debian/Ubuntu 缺失 | **Tier 3** | 内嵌为 `3rdparty/readerwriterqueue/` (单头文件, ~600行) |
| **零拷贝 mmap** (#412) | `mio` | ⚠️ Debian 缺失 | **Tier 3** | 内嵌为 `3rdparty/mio/` (单头文件, ~800行) |
| **CLI 终端表格排版** (#420)| `tabulate` / `indicators` | ⚠️ 发行版覆盖不全 | **Tier 3** | 内嵌为 `3rdparty/tabulate/` (纯展示单头文件) |

---

## 5. CMake 与工程落地标准 SOP

### 5.1 根目录 `CMakeLists.txt`：极致瘦身与单行约定

彻底拔除以往多达百余行的 `FetchContent` 冗余代码。通用库全部强制走单行：

```cmake
# 系统核心与通用现代库（全发行版预装，缺失直接报错引导安装）
find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Config REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(spdlog REQUIRED)
pkg_check_modules(SDBUS REQUIRED libsystemd)
find_package(PipeWire REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(LibArchive REQUIRED)

# 统一接入本地 3rdparty 内嵌库（零网络请求，极简单行）
add_subdirectory(3rdparty)
```

### 5.2 `3rdparty/` 目录组织与 `SYSTEM INTERFACE` 隔离

1. **目录结构规范**：
   ```text
   3rdparty/
   ├── CMakeLists.txt
   ├── <lib_name>/
   │   ├── <lib_name>.hpp (或 .h)
   │   └── LICENSE (必须完整保留上游开源协议文本)
   ```

2. **`3rdparty/CMakeLists.txt` 统一封装规范**：
   ```cmake
   # 针对完全无系统包的轻量单头文件库（如 base64, unicodelib）
   add_library(3rdparty_base64 INTERFACE)
   target_include_directories(3rdparty_base64 SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/base64)
   add_library(base64::base64 ALIAS 3rdparty_base64)

   add_library(3rdparty_unicodelib INTERFACE)
   target_include_directories(3rdparty_unicodelib SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/unicodelib)
   add_library(unicodelib::unicodelib ALIAS 3rdparty_unicodelib)

   # 针对部分发行版有包的库（如 httplib）：系统优先，本地 3rdparty 零网络兜底
   find_package(httplib QUIET)
   if(httplib_FOUND)
       message(STATUS "Using system httplib")
   else()
       message(STATUS "Using bundled httplib from 3rdparty")
       add_library(3rdparty_httplib INTERFACE)
       target_include_directories(3rdparty_httplib SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/httplib)
       add_library(httplib::httplib ALIAS 3rdparty_httplib)
   endif()
   ```

3. **开源法务与合规登记 (`THIRD_PARTY_NOTICES.md`)**：
   每纳入一个 `3rdparty` 库，必须在根目录 `THIRD_PARTY_NOTICES.md` 登记：
   - 库名称与 Upstream GitHub 地址；
   - 纳入的 Commit/Tag 版本及导入日期；
   - 开源许可证（如 MIT、Apache-2.0 等）。

---

## 6. 工具层重构准则：杜绝手写与过度封装

1. **零手写底层基础设施**：
   - 不再手写底层状态机、查找表、字符大小写位运算、码点区间表；
   - UTF-8 字宽、字符分类、CJK 判定全部交由成熟 Unicode 库（`unicodelib`）标准查表。
2. **拒绝无意义的多重转发门面（YAGNI 原则）**：
   - 单点业务使用（例如 Base64 仅用于流式 PCM 转 JSON）：直接在业务调用处引入头文件使用，不在公共工具类中维护多重无意义重载包装；
   - 公共工具类（如 `string_utils.h`）仅保留真正的全项目共享能力，代码量严格收敛；
3. **复合算法才进行封装**：
   - 仅对跨库组装的高阶复合业务算法（如同时依赖“字形簇边界探测 + 界面宽度预算分配 + 居中插入省略号”的 `TruncateMiddleUtf8`）进行精简封装，内部逻辑完全由成熟外部库能力驱动。
