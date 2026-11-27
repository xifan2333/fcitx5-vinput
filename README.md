<div align="center">

# fcitx5-vinput

**Voice input for Fcitx5 — local and cloud ASR, LLM rewriting, cross-distro packages**

[![License](https://img.shields.io/github/license/fcitx5-vinput/fcitx5-vinput)](LICENSE)
[![CI](https://github.com/fcitx5-vinput/fcitx5-vinput/actions/workflows/release.yml/badge.svg)](https://github.com/fcitx5-vinput/fcitx5-vinput/actions/workflows/release.yml)
[![Release](https://img.shields.io/github/v/release/fcitx5-vinput/fcitx5-vinput)](https://github.com/fcitx5-vinput/fcitx5-vinput/releases)
[![AUR](https://img.shields.io/aur/version/fcitx5-vinput-bin)](https://aur.archlinux.org/packages/fcitx5-vinput-bin)
[![Downloads](https://img.shields.io/github/downloads/fcitx5-vinput/fcitx5-vinput/total)](https://github.com/fcitx5-vinput/fcitx5-vinput/releases)

[English](README.md) | [中文](README_zh.md) | [Documentation](https://fcitx5-vinput.github.io/)

https://github.com/user-attachments/assets/5a548a68-153c-4842-bab6-926f30bb720e

</div>

## Features

- **Two trigger modes** — tap to toggle recording, or hold to push-to-talk
- **Local & cloud ASR** — offline [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) models or cloud providers (Doubao, Aliyun Bailian, ElevenLabs, OpenAI-compatible), switchable at runtime from the command palette (`Shift_R` → `/asr`)
- **LLM post-processing** — error correction, formatting, translation via scenes
- **Command mode** — select text, speak an instruction, release to apply
- **GUI & CLI** — `vinput-gui` for quick setup, `vinput` CLI for full control
- **Cross-distro** — Arch, Fedora, Ubuntu/Debian, Nix, Flatpak

## Installation

### Arch Linux ([archlinuxcn](https://www.archlinuxcn.org/archlinux-cn-repo-and-mirror/) / AUR)

If you use the [archlinuxcn](https://www.archlinuxcn.org/archlinux-cn-repo-and-mirror/) repository, install the repo-built package:

```bash
sudo pacman -S fcitx5-vinput
```

The AUR binary package is also available:

```bash
# Full version (with local sherpa-onnx runtime)
yay -S fcitx5-vinput-bin

# Lite version (cloud-only, zero local ONNX runtime)
yay -S fcitx5-vinput-lite-bin
```

### Fedora (COPR)

```bash
sudo dnf copr enable xifan/fcitx5-vinput-bin

# Full version (with local sherpa-onnx runtime)
sudo dnf install fcitx5-vinput

# Lite version (cloud-only, zero local ONNX runtime)
sudo dnf install fcitx5-vinput-lite
```

### Ubuntu 24.04 (PPA)

```bash
sudo add-apt-repository ppa:xifan233/ppa
sudo apt update

# Full version (with local sherpa-onnx runtime)
sudo apt install fcitx5-vinput

# Lite version (cloud-only, zero local ONNX runtime)
sudo apt install fcitx5-vinput-lite
```

### Ubuntu / Debian (manual)

```bash
# Download latest .deb from GitHub Releases
# Full version (with local sherpa-onnx runtime):
sudo dpkg -i fcitx5-vinput_*.deb
sudo apt-get install -f

# Lite version (cloud-only, zero local ONNX runtime):
sudo dpkg -i fcitx5-vinput-lite_*.deb
sudo apt-get install -f
```

### Nix (flake)

Supports `x86_64-linux` and `aarch64-linux`. Both full and lite packages are cached on Cachix.

```nix
inputs.fcitx5-vinput.url = "github:fcitx5-vinput/fcitx5-vinput";

# Full version (default):
#   inputs.fcitx5-vinput.packages.${system}.default
# Lite version (cloud-only, zero local ONNX runtime):
#   inputs.fcitx5-vinput.packages.${system}.fcitx5-vinput-lite
```

Binary cache via [Cachix](https://fcitx5-vinput.cachix.org):

```nix
nixConfig = {
  extra-substituters = [ "https://fcitx5-vinput.cachix.org" ];
  extra-trusted-public-keys = [ "fcitx5-vinput.cachix.org-1:XpX3AA6+dDIX4qJhb1QM7sbTwX6/qSlGvW8Z5NK6XdU=" ];
};
```

Full Home Manager example in the [install docs](https://fcitx5-vinput.github.io/install/).

### Flatpak

```bash
flatpak remote-add --if-not-exists xifan https://xifan2333.github.io/flatpak-auto/xifan.flatpakrepo

# Full version (with local sherpa-onnx runtime)
flatpak install https://xifan2333.github.io/flatpak-auto/refs/org.fcitx.Fcitx5.Addon.Vinput.flatpakref

# Lite version (cloud-only, zero local ONNX runtime)
flatpak install https://xifan2333.github.io/flatpak-auto/refs/org.fcitx.Fcitx5.Addon.Vinput.Lite.flatpakref
```

After installation, grant the extra permissions and restart Fcitx5:

```bash
flatpak override --user --socket=pulseaudio org.fcitx.Fcitx5
flatpak override --user --filesystem=xdg-config/systemd:create org.fcitx.Fcitx5
flatpak override --user --filesystem=xdg-cache org.fcitx.Fcitx5
flatpak kill org.fcitx.Fcitx5
```

### GitHub Releases

Download the package for your system from [GitHub Releases](https://github.com/fcitx5-vinput/fcitx5-vinput/releases/latest) (both full and lite variants are provided):

| Package | Applicable Systems | Notes |
|---------|--------------------|-------|
| `*ubuntu24.04_{amd64,arm64}.deb` | Ubuntu 24.04+, Linux Mint 22+ | glibc 2.39+, Qt 6 |
| `*debian12_{amd64,arm64}.deb` | Debian 12+ and Debian 12-based distros | glibc 2.36+, Qt 6 |
| `*debian11_{amd64,arm64}.deb` | Debian 11, Kylin V10 SP1, UOS V20, and other legacy Debian-based distros | glibc 2.31+, Qt 5 (`>= 5.12`) |
| `*.rpm` | Fedora 43+, openSUSE Leap 15.6+ | Qt 6 |
| `*.pkg.tar.zst` | Arch Linux and Arch-based distros | Qt 6 |
| `*.flatpak` | Systems running Flatpak `org.fcitx.Fcitx5` | Sandboxed addon |
| `*_linux_{x86_64,aarch64}_*.tar.gz` | Generic Linux (`x86_64` / `aarch64`) | Built on Ubuntu 24.04 baseline |

### Minimum Support Baseline & Build from Source

- **Minimum runtime baseline:** `glibc 2.31+` (`GLIBCXX_3.4.28+`), `PulseAudio` or `pipewire-pulse` (via `libpulse`), `Fcitx5 5.0.5+`, `Qt 5.12+` or `Qt 6.x`
- **Build dependencies:** `cmake`, `fcitx5 (>= 5.0.5)`, `libpulse`, `libcurl`, `libarchive`, `libsystemd`, `Qt 5 (>= 5.12)` or `Qt 6` (default `auto` prefers Qt 6 and falls back to Qt 5; override with `-DVINPUT_QT_MAJOR=6|5`)

```bash
# Full version (with local sherpa-onnx ASR runtime)
sudo bash scripts/build-sherpa-onnx.sh
cmake --preset release-clang-mold
cmake --build --preset release-clang-mold
sudo cmake --install build

# Lite version (pure cloud ASR + LLM, zero sherpa-onnx dependency)
cmake --preset release-clang-mold -DVINPUT_ENABLE_LOCAL_ASR=OFF
cmake --build --preset release-clang-mold
sudo cmake --install build
```

## Quick start

```bash
systemctl --user enable --now vinput-daemon.service
fcitx5 -r
```

Open **Vinput GUI** → **Resources → Models** → download and activate a model. Then:

- **Tap** `Alt_R` — start/stop recording
- **Hold** `Alt_R` — push-to-talk

## Key bindings

| Key | Default | Function |
|-----|---------|----------|
| Trigger Key | `Alt_R` | Tap to toggle recording; hold to push-to-talk |
| Command Key | `Control_R` | Hold after selecting text to modify with voice |
| Command Palette Key | `Shift_R` | Open unified command palette (/model, /asr, /scene, /proc) |

All keys can be customized in Fcitx5 configuration.

## Documentation

For ASR configuration, scenes & LLM setup, CLI reference, and registry contribution guide, see the [documentation site](https://fcitx5-vinput.github.io/).

## License

[GPL-3.0](LICENSE)

## Contributors

<a href="https://github.com/fcitx5-vinput/fcitx5-vinput/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=fcitx5-vinput/fcitx5-vinput" />
</a>

## Sponsor

If this project has been helpful to you, feel free to support it.

<img src="https://raw.githubusercontent.com/xifan2333/xifan2333/main/assets/donate.png" alt="Donate" width="300" />
