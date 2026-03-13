# fcitx5-vinput

[English](README.md) | [中文](README_zh.md)

基于 Fcitx5 的本地离线语音输入插件，使用 sherpa-onnx 进行语音识别，支持 LLM 后处理。

## 功能

- 按住触发键录音，松开后自动识别并上屏
- 支持 LLM 后处理（纠错、格式化、翻译等），兼容 OpenAI API
- 命令模式：选中文本后按命令键，语音指令直接修改选中内容
- 场景管理：为不同场景配置不同的后处理 prompt
- 多 LLM provider：可配置多个服务端，随时切换
- 热词支持（部分模型）
- `vinput` CLI 工具：管理模型、场景、LLM 配置

## 安装

### Arch Linux

```bash
# 从 GitHub Releases 下载最新 .pkg.tar.zst
sudo pacman -U fcitx5-vinput-*.pkg.tar.zst
```

### Ubuntu / Debian

```bash
# 从 GitHub Releases 下载最新 .deb
sudo dpkg -i fcitx5-vinput_*.deb
sudo apt-get install -f
```

### 从源码编译

依赖：`cmake`、`fcitx5`、`sherpa-onnx`、`pipewire`、`libcurl`、`nlohmann-json`、`CLI11`、`Qt6`

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

## 快速开始

### 1. 安装模型

```bash
vinput model list --remote      # 查看可用模型
vinput model add <模型名>        # 下载并安装
vinput model use <模型名>        # 设置为当前模型
```

也可手动将模型目录放到 `~/.local/share/fcitx5-vinput/models/<模型名>/`，目录内需包含：

- `vinput-model.json`
- `model.int8.onnx` 或 `model.onnx`
- `tokens.txt`

### 2. 启动守护进程

```bash
systemctl --user enable --now vinput-daemon.service
```

### 3. 在 Fcitx5 中启用

打开 Fcitx5 配置 → 附加组件 → 找到 **Vinput** → 启用。

### 4. 开始使用

按住触发键（默认 `Alt_R`）开始录音，松开后识别结果自动上屏。

## 按键说明

| 按键 | 默认 | 功能 |
|------|------|------|
| 触发键 | `Alt_R` | 按住录音，松开识别上屏 |
| 命令键 | `Control_R` | 选中文本后按住，语音指令修改选中内容 |
| 场景菜单键 | `Shift_R` | 打开后处理场景选单 |
| 翻页 | `Page Up` / `Page Down` | 候选列表翻页 |
| 移动 | `↑` / `↓` | 候选列表移动光标 |
| 确认 | `Enter` | 确认选中候选 |
| 取消 | `Esc` | 关闭菜单 |
| 快选 | `1`–`9` | 快速选择候选 |

所有按键均可在 Fcitx5 配置界面中自定义。

## 配置

### 图形界面

在 Fcitx5 配置中打开 Vinput 附加组件，或直接运行：

```bash
vinput-gui
```

### CLI 工具

#### 模型管理

```bash
vinput model list               # 列出已安装模型
vinput model list --remote      # 列出可用远程模型
vinput model add <名称>          # 下载安装模型
vinput model use <名称>          # 切换当前模型
vinput model remove <名称>       # 删除模型
vinput model info <名称>         # 查看模型详情
```

#### 场景管理

```bash
vinput scene list               # 列出所有场景
vinput scene add                # 添加场景（交互式）
vinput scene edit               # 编辑场景
vinput scene use <ID>           # 切换当前场景
vinput scene remove <ID>        # 删除场景
```

#### LLM 配置

```bash
vinput llm list                 # 列出所有 provider
vinput llm add                  # 添加 provider（交互式）
vinput llm edit                 # 编辑 provider
vinput llm use <名称>            # 切换当前 provider
vinput llm remove <名称>         # 删除 provider
vinput llm enable               # 启用 LLM 后处理
vinput llm disable              # 禁用 LLM 后处理
```

#### 热词管理

```bash
vinput hotword get              # 查看当前热词文件路径
vinput hotword set <路径>        # 设置热词文件
vinput hotword edit             # 用编辑器打开热词文件
vinput hotword clear            # 清除热词文件配置
```

#### 守护进程管理

```bash
vinput daemon status            # 查看 daemon 状态
vinput daemon start             # 启动
vinput daemon stop              # 停止
vinput daemon restart           # 重启
vinput daemon logs              # 查看日志
```

## 场景

场景控制 LLM 对识别结果的处理方式，通过场景菜单键在运行时切换。

每个场景包含：

- **ID**：唯一标识符
- **标签**：菜单中显示的名称
- **Prompt**：发送给 LLM 的系统提示

`default` 场景与其他场景一样会调用 LLM（如果已启用）。若要跳过 LLM，可全局禁用 LLM，或将场景的候选数设置为 0。

## 命令模式

先选中一段文本，再按住命令键说出指令，松开后 LLM 根据指令修改选中内容并替换上屏。

示例：
- 选中一段中文 → 按住命令键 → 说「翻译成英文」→ 松开 → 选中内容被替换为英文译文
- 选中代码 → 说「加上注释」→ 松开 → 代码被替换为加注释版本

命令模式依赖 LLM，需要先配置并启用 LLM provider。

## LLM 配置示例

以本地 Ollama 为例：

```bash
vinput llm add
# 按提示填写：
# 名称: ollama
# Base URL: http://127.0.0.1:11434/v1
# API Key: （留空）
# Model: qwen2.5:7b

vinput llm use ollama
vinput llm enable
```

## 配置文件位置

| 文件 | 路径 |
|------|------|
| Fcitx5 插件配置（按键等） | `~/.config/fcitx5/conf/vinput.conf` |
| 核心配置（模型、LLM、场景） | `~/.config/vinput/config.json` |
| 模型目录 | `~/.local/share/fcitx5-vinput/models/` |

## 打包发布

推送形如 `v0.1.0` 的 tag 后，GitHub Actions 会自动构建并上传以下产物到 Release：

- 源码包 `fcitx5-vinput-<version>.tar.gz`
- Ubuntu 24.04 `.deb`
- Arch Linux `.pkg.tar.zst`
