<div align="center">

# fcitx5-vinput

**Voice input for Fcitx5 — local and cloud ASR, LLM rewriting, cross-distro packages**

[![License](https://img.shields.io/github/license/xifan2333/fcitx5-vinput)](LICENSE)
[![CI](https://github.com/xifan2333/fcitx5-vinput/actions/workflows/release.yml/badge.svg)](https://github.com/xifan2333/fcitx5-vinput/actions/workflows/release.yml)
[![Release](https://img.shields.io/github/v/release/xifan2333/fcitx5-vinput)](https://github.com/xifan2333/fcitx5-vinput/releases)
[![AUR](https://img.shields.io/aur/version/fcitx5-vinput-bin)](https://aur.archlinux.org/packages/fcitx5-vinput-bin)
[![Downloads](https://img.shields.io/github/downloads/xifan2333/fcitx5-vinput/total)](https://github.com/xifan2333/fcitx5-vinput/releases)

[English](README.md) | [中文](README_zh.md) | [Documentation](https://xifan2333.github.io/fcitx5-vinput/)

https://github.com/user-attachments/assets/5a548a68-153c-4842-bab6-926f30bb720e

</div>

## Features

- **Two trigger modes** — tap to toggle recording, or hold to push-to-talk
- **Local & cloud ASR** — offline [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) models or cloud providers (Doubao, Aliyun Bailian, ElevenLabs, OpenAI-compatible), switchable at runtime with `F8`
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
inputs.fcitx5-vinput.url = "github:xifan2333/fcitx5-vinput";

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

Full Home Manager example in the [install docs](https://xifan2333.github.io/fcitx5-vinput/install/).

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
flatpak override --user --filesystem=xdg-run/pipewire-0 org.fcitx.Fcitx5
flatpak override --user --filesystem=xdg-config/systemd:create org.fcitx.Fcitx5
flatpak override --user --filesystem=xdg-cache org.fcitx.Fcitx5
flatpak kill org.fcitx.Fcitx5
```

### GitHub Releases

Download the package for your system from [GitHub Releases](https://github.com/xifan2333/fcitx5-vinput/releases/latest) (both full and lite variants are provided):

- **Debian / Linux Mint / Ubuntu (other)**: `.deb` (`fcitx5-vinput_*.deb` / `fcitx5-vinput-lite_*.deb`)
- **openSUSE / Fedora (other)**: `.rpm` (`fcitx5-vinput-*.rpm` / `fcitx5-vinput-lite-*.rpm`)
- **Arch-based**: `.pkg.tar.zst` (`fcitx5-vinput-*.pkg.tar.zst` / `fcitx5-vinput-lite-*.pkg.tar.zst`)
- **Flatpak**: `.flatpak` (`fcitx5-vinput.flatpak` / `fcitx5-vinput-lite.flatpak`)
- **Generic Linux**: `tar.gz` (`*_bundled.tar.gz` / `fcitx5-vinput-lite-*.tar.gz`)

### Build from source

**Dependencies:** cmake, fcitx5, pipewire, libcurl, nlohmann-json, CLI11, Qt6

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

For ASR configuration, scenes & LLM setup, CLI reference, and registry contribution guide, see the [documentation site](https://xifan2333.github.io/fcitx5-vinput/).

## License

[GPL-3.0](LICENSE)

## Contributors

<a href="https://github.com/xifan2333/fcitx5-vinput/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=xifan2333/fcitx5-vinput" />
</a>

## Sponsor

If this project has been helpful to you, feel free to support it.

<img src="https://raw.githubusercontent.com/xifan2333/xifan2333/main/assets/donate.png" alt="Donate" width="300" />
