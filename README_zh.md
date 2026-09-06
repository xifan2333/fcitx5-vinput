<div align="center">

# fcitx5-vinput

**Fcitx5 语音输入方案 — 本地与云端 ASR、LLM 改写、多发行版支持**

[![License](https://img.shields.io/github/license/xifan2333/fcitx5-vinput)](LICENSE)
[![CI](https://github.com/xifan2333/fcitx5-vinput/actions/workflows/release.yml/badge.svg)](https://github.com/xifan2333/fcitx5-vinput/actions/workflows/release.yml)
[![Release](https://img.shields.io/github/v/release/xifan2333/fcitx5-vinput)](https://github.com/xifan2333/fcitx5-vinput/releases)
[![AUR](https://img.shields.io/aur/version/fcitx5-vinput-bin)](https://aur.archlinux.org/packages/fcitx5-vinput-bin)
[![Downloads](https://img.shields.io/github/downloads/xifan2333/fcitx5-vinput/total)](https://github.com/xifan2333/fcitx5-vinput/releases)

[English](README.md) | [中文](README_zh.md) | [文档站点](https://xifan2333.github.io/fcitx5-vinput/zh-cn/)

https://github.com/user-attachments/assets/5a548a68-153c-4842-bab6-926f30bb720e

</div>

## 功能特性

- **两种触发模式** — 短按切换录音，长按即说即停（push-to-talk）
- **本地与云端 ASR** — 离线 [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) 模型或云端提供商（豆包、阿里百炼、ElevenLabs、OpenAI 兼容），右 Shift 打开命令面板后用 `/asr` 运行时切换
- **LLM 后处理** — 通过场景实现纠错、格式化、翻译
- **命令模式** — 选中文本，语音指令直接改写
- **GUI 与 CLI** — `vinput-gui` 快速上手，`vinput` CLI 完整控制
- **多发行版** — Arch、Fedora、Ubuntu/Debian、Nix、Flatpak

## 安装

### Arch Linux（[archlinuxcn](https://www.archlinuxcn.org/archlinux-cn-repo-and-mirror/) / AUR）

如果已启用 [archlinuxcn](https://www.archlinuxcn.org/archlinux-cn-repo-and-mirror/) 仓库，可以直接安装仓库构建包：

```bash
sudo pacman -S fcitx5-vinput
```

也可以使用 AUR 二进制包：

```bash
# 完整版（带本地 sherpa-onnx 离线推理）
yay -S fcitx5-vinput-bin

# 极简 Lite 版（纯云端 ASR + LLM，无本地 ONNX 运行时）
yay -S fcitx5-vinput-lite-bin
```

### Fedora (COPR)

```bash
sudo dnf copr enable xifan/fcitx5-vinput-bin

# 完整版（带本地 sherpa-onnx 离线推理）
sudo dnf install fcitx5-vinput

# 极简 Lite 版（纯云端 ASR + LLM，无本地 ONNX 运行时）
sudo dnf install fcitx5-vinput-lite
```

### Ubuntu 24.04 (PPA)

```bash
sudo add-apt-repository ppa:xifan233/ppa
sudo apt update

# 完整版（带本地 sherpa-onnx 离线推理）
sudo apt install fcitx5-vinput

# 极简 Lite 版（纯云端 ASR + LLM，无本地 ONNX 运行时）
sudo apt install fcitx5-vinput-lite
```

### Ubuntu / Debian（手动安装）

```bash
# 从 GitHub Releases 下载最新 .deb
# 完整版（带本地 sherpa-onnx 离线推理）：
sudo dpkg -i fcitx5-vinput_*.deb
sudo apt-get install -f

# 极简 Lite 版（纯云端 ASR + LLM，无本地 ONNX 运行时）：
sudo dpkg -i fcitx5-vinput-lite_*.deb
sudo apt-get install -f
```

### Nix (flake)

支持 `x86_64-linux` 和 `aarch64-linux`。完整版与 Lite 版均在 Cachix 预编译缓存：

```nix
inputs.fcitx5-vinput.url = "github:xifan2333/fcitx5-vinput";

# 完整版（默认）：
#   inputs.fcitx5-vinput.packages.${system}.default
# 极简 Lite 版（纯云端 ASR + LLM，零 ONNX 依赖）：
#   inputs.fcitx5-vinput.packages.${system}.fcitx5-vinput-lite
```

通过 [Cachix](https://fcitx5-vinput.cachix.org) 提供二进制缓存：

```nix
nixConfig = {
  extra-substituters = [ "https://fcitx5-vinput.cachix.org" ];
  extra-trusted-public-keys = [ "fcitx5-vinput.cachix.org-1:XpX3AA6+dDIX4qJhb1QM7sbTwX6/qSlGvW8Z5NK6XdU=" ];
};
```

完整 Home Manager 示例见[安装文档](https://xifan2333.github.io/fcitx5-vinput/zh-cn/install/)。

### Flatpak

```bash
flatpak remote-add --if-not-exists xifan https://xifan2333.github.io/flatpak-auto/xifan.flatpakrepo

# 完整版（带本地 sherpa-onnx 离线推理）
flatpak install https://xifan2333.github.io/flatpak-auto/refs/org.fcitx.Fcitx5.Addon.Vinput.flatpakref

# 极简 Lite 版（纯云端 ASR + LLM，零本地 ONNX 运行时）
flatpak install https://xifan2333.github.io/flatpak-auto/refs/org.fcitx.Fcitx5.Addon.Vinput.Lite.flatpakref
```

安装后需要授予额外权限并重启 Fcitx5：

```bash
flatpak override --user --filesystem=xdg-run/pipewire-0 org.fcitx.Fcitx5
flatpak override --user --filesystem=xdg-config/systemd:create org.fcitx.Fcitx5
flatpak override --user --filesystem=xdg-cache org.fcitx.Fcitx5
flatpak kill org.fcitx.Fcitx5
```

### GitHub Releases

从 [GitHub Releases](https://github.com/xifan2333/fcitx5-vinput/releases/latest) 下载对应安装包（均提供完整版与 Lite 极简版）：

- **Debian / Linux Mint / Ubuntu（其他版本）**：`.deb`（`fcitx5-vinput_*.deb` / `fcitx5-vinput-lite_*.deb`）
- **openSUSE / Fedora（其他版本）**：`.rpm`（`fcitx5-vinput-*.rpm` / `fcitx5-vinput-lite-*.rpm`）
- **Arch 系**：`.pkg.tar.zst`（`fcitx5-vinput-*.pkg.tar.zst` / `fcitx5-vinput-lite-*.pkg.tar.zst`）
- **Flatpak**：`.flatpak`（`fcitx5-vinput.flatpak` / `fcitx5-vinput-lite.flatpak`）
- **通用 Linux**：`tar.gz`（`*_bundled.tar.gz` / `fcitx5-vinput-lite-*.tar.gz`）

### 源码构建

**依赖：** cmake、fcitx5、pipewire、libcurl、nlohmann-json、CLI11、Qt6

```bash
# 完整版（带本地 sherpa-onnx 离线推理引擎）
sudo bash scripts/build-sherpa-onnx.sh
cmake --preset release-clang-mold
cmake --build --preset release-clang-mold
sudo cmake --install build

# 极简 Lite 版（纯云端 ASR + LLM，零 sherpa-onnx 依赖）
cmake --preset release-clang-mold -DVINPUT_ENABLE_LOCAL_ASR=OFF
cmake --build --preset release-clang-mold
sudo cmake --install build
```

## 快速开始

```bash
systemctl --user enable --now vinput-daemon.service
fcitx5 -r
```

打开 **Vinput GUI** → **资源 → 模型** → 下载并激活一个模型。然后：

- **短按** `Alt_R` — 开始/停止录音
- **长按** `Alt_R` — 即说即停

## 按键说明

| 按键 | 默认 | 功能 |
|------|------|------|
| 触发键 | `Alt_R` | 短按切换录音；长按即说即停 |
| 命令键 | `Control_R` | 选中文本后按住，语音指令修改选中内容 |
| 命令面板键 | `Shift_R` | 打开统一命令面板（/model、/asr、/scene、/proc） |

所有按键均可在 Fcitx5 配置界面中自定义。

## 文档

ASR 配置、场景与 LLM、CLI 参考和资源仓库贡献规范请查看[文档站点](https://xifan2333.github.io/fcitx5-vinput/zh-cn/)。

## 许可证

[GPL-3.0](LICENSE)

## 贡献者

<a href="https://github.com/xifan2333/fcitx5-vinput/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=xifan2333/fcitx5-vinput" />
</a>

## 赞赏

如果这个项目对你有帮助，欢迎赞赏支持。

<img src="https://raw.githubusercontent.com/xifan2333/xifan2333/main/assets/donate.png" alt="赞赏码" width="300" />
