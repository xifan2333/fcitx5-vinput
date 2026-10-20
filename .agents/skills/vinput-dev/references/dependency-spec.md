# 第三方依赖管理与集成规范

本项目对于外部依赖遵循**系统包优先、按需内嵌、绝对离线、源码零篡改**的原则。

---

## 1. 核心铁律 (Hard Constraints)

1. **构建期零网络 (Zero Network during Build)**：
   构建系统绝对不充当下载器。严禁在构建脚本中使用 `FetchContent`、`git clone` 或 HTTP 下载；所有依赖要么由发行版系统 dev 包提供，要么自包含于本地源码中。保证 Nix 沙箱、Flatpak 离线构建与 Arch clean chroot 100% 确定性。
2. **发行版通用包非必要不内嵌 (No Unbundling)**：
   凡目标发行版官方已稳定收录的通用库，强制走系统包分发，严禁复制到仓库中。
3. **CMake `SYSTEM INTERFACE` 强制隔离**：
   内嵌库必须在 CMake 中以 `SYSTEM INTERFACE` 声明，令编译器和静态分析器将其视为系统头文件，天然豁免 `clang-tidy` 诊断与编译告警。
4. **3rdparty 源码绝对零篡改 (Pristine Vendoring)**：
   内嵌至 `3rdparty/` 的第三方源码必须保持 100% 原始上游快照。严禁任何人工修补、改动、重构或格式化；所有业务接口适配与兼容胶水一律放在 `src/common/` 或 `src/daemon/`，严禁侵入 `3rdparty/`。
5. **开源许可证合规登记**：
   每引入一个内嵌库，必须在根目录 `THIRD_PARTY_NOTICES.md` 中完整登记其名称、上游地址、版本、导入日期与许可证文本。

---

## 2. 三层优先级阶梯与准入决策 (Three-Tier Hierarchy)

引入新依赖必须经过优先级判定阶梯，从高到低决策：

| 梯队 | 定位与策略 | 适用范围与典型代表 | 构建与打包标准 |
| :--- | :--- | :--- | :--- |
| **Tier 1: 宿主系统核心服务** | 仅由系统包管理器提供，严禁内嵌。 | `fcitx5`, `pipewire`, `libsystemd`, `qt6`, `openssl` | CMake 单行 `find_package(... REQUIRED)` 或 `pkg_check_modules`。 |
| **Tier 2: 全发行版通用库** | 目标发行版全覆盖的现代 C++ 库。系统包分发，不进仓库。 | `spdlog`, `nlohmann_json`, `CLI11`, `reproc` | 1. 在 `scripts/ci-install-build-deps.sh` 声明对应发行版包名；<br>2. 在 `flake.nix` 的 `buildInputs` 中补充声明；<br>3. CMake 单行 `find_package(... REQUIRED)`。 |
| **Tier 3: 生态缺失专用库** | 主流发行版未收录或覆盖不全的轻量单头文件库。 | `httplib`, `base64`, `unicodelib`, `readerwriterqueue`, `mio` | 内嵌于 `3rdparty/`，由 `3rdparty/CMakeLists.txt` 统管，声明 `SYSTEM INTERFACE`。 |

### 准入决策树

```text
引入新第三方依赖
       │
       ▼
目标发行版（Debian / Ubuntu / Fedora / openSUSE / Arch / Nixpkgs）均有官方打包？
       │
       ├─────► 【是】──────► 归入 Tier 2（系统级分发）
       │                     1. 维护 ci-install-build-deps.sh 与 flake.nix 声明系统包
       │                     2. CMake 单行 find_package(xxx REQUIRED)
       │                     3. 严禁复制代码进仓库，严禁写 Fetch 兜底
       │
       ▼ 【否：主流发行版缺失（如缺少官方包、或纯 Header 库无独立包）】
必须满足内嵌准入条件：
1. 必须是轻量、专用的单头文件库（Header-Only）或极小型源码库
2. 具有清晰明确的宽松开源协议（MIT, Apache-2.0, BSD）
       │
       └──────────────────► 归入 Tier 3（本地内嵌 3rdparty/）
                            1. 存放于 3rdparty/<lib_name>/
                            2. 由 3rdparty/CMakeLists.txt 封装 SYSTEM INTERFACE
                            3. 在 THIRD_PARTY_NOTICES.md 登记合规信息
```

---

## 3. Tier 3 内嵌库落地 SOP

### 3.1 目录组织结构
```text
3rdparty/
├── CMakeLists.txt
└── <lib_name>/
    ├── <lib_name>.hpp (或 .h，严格保持上游原样)
    └── LICENSE (完整保留上游开源许可文本)
```

### 3.2 `3rdparty/CMakeLists.txt` 隔离封装规范
使用 `SYSTEM INTERFACE` 阻断告警传递：

```cmake
# 纯内嵌头文件库范例（如 base64, unicodelib）
add_library(3rdparty_<name> INTERFACE)
target_include_directories(3rdparty_<name> SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/<name>)
add_library(<name>::<name> ALIAS 3rdparty_<name>)

# 支持系统优先探测、本地内嵌优雅回退的库范例（如 httplib）
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

### 3.3 合规登记 (`THIRD_PARTY_NOTICES.md`)
新增内嵌库必须在 `THIRD_PARTY_NOTICES.md` 追加标准段落：
- 库名称与 Upstream GitHub URL；
- 纳入时的特定 Commit Hash 或 Tag，以及导入日期；
- 完整的许可证全文或标识。

---

## 4. 业务层消费与包装原则

1. **禁造轮子**：
   字符分类、CJK/东亚字宽判定、Base64 编解码、跨平台 mmap 等基础能力，必须直接消费已引入的标准库（如 `unicodelib`, `base64`, `mio`），严禁手写区间查找表或位运算状态机。
2. **拒绝浅门面包装 (No Shallow Facades)**：
   单点业务使用（例如仅在某处做 JSON 转 Base64）：直接在业务实现文件中 `#include` 库头文件使用。不要为第三方库书写无业务增值的简单转发函数。
3. **高阶复合逻辑才集中封装**：
   仅当涉及跨库、跨模块的复合领域逻辑（例如“字形簇边界探测 + 界面宽度预算分配 + 居中插入省略号”的字符串智能截断算法）时，才在 `src/common/utils/` 下抽象为纯函数工具。
