# fcitx5-vinput

[English](README.md) | [中文](README_zh.md)

Local offline voice input plugin for Fcitx5, powered by sherpa-onnx for speech recognition with LLM post-processing support.

## Features

- Press and hold trigger key to record, release to recognize and commit text
- LLM post-processing support (error correction, formatting, translation, etc.) with OpenAI API compatibility
- Command mode: Select text, press command key, speak instructions to modify selected content
- Scene management: Configure different post-processing prompts for different scenarios
- Multiple LLM providers: Configure multiple servers and switch between them
- Hotword support (for compatible models)
- `vinput` CLI tool: Manage models, scenes, and LLM configurations

## Installation

### Arch Linux

```bash
# Download latest .pkg.tar.zst from GitHub Releases
sudo pacman -U fcitx5-vinput-*.pkg.tar.zst
```

### Ubuntu / Debian

```bash
# Download latest .deb from GitHub Releases
sudo dpkg -i fcitx5-vinput_*.deb
sudo apt-get install -f
```

### Build from Source

Dependencies: `cmake`, `fcitx5`, `sherpa-onnx`, `pipewire`, `libcurl`, `nlohmann-json`, `CLI11`, `Qt6`

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

## Quick Start

### 1. Install Model

```bash
vinput model list --remote      # List available models
vinput model add <model-name>   # Download and install
vinput model use <model-name>   # Set as current model
```

Or manually place model directory in `~/.local/share/fcitx5-vinput/models/<model-name>/`, containing:

- `vinput-model.json`
- `model.int8.onnx` or `model.onnx`
- `tokens.txt`

### 2. Start Daemon

```bash
systemctl --user enable --now vinput-daemon.service
```

### 3. Enable in Fcitx5

Open Fcitx5 Configuration → Addons → Find **Vinput** → Enable.

### 4. Start Using

Press and hold trigger key (default `Alt_R`) to record, release to recognize and commit.

## Key Bindings

| Key | Default | Function |
|-----|---------|----------|
| Trigger Key | `Alt_R` | Press to record, release to recognize |
| Command Key | `Control_R` | Select text, press to modify with voice command |
| Scene Menu Key | `Shift_R` | Open post-processing scene menu |
| Page Up/Down | `Page Up` / `Page Down` | Navigate candidate list |
| Move | `↑` / `↓` | Move cursor in candidate list |
| Confirm | `Enter` | Confirm selected candidate |
| Cancel | `Esc` | Close menu |
| Quick Select | `1`–`9` | Quick select candidate |

All keys can be customized in Fcitx5 configuration.

## Configuration

### GUI

Open Vinput addon in Fcitx5 configuration, or run directly:

```bash
vinput-gui
```

### CLI Tool

#### Model Management

```bash
vinput model list               # List installed models
vinput model list --remote      # List available remote models
vinput model add <name>         # Download and install model
vinput model use <name>         # Switch current model
vinput model remove <name>      # Remove model
vinput model info <name>        # View model details
```

#### Scene Management

```bash
vinput scene list               # List all scenes
vinput scene add                # Add scene (interactive)
vinput scene edit               # Edit scene
vinput scene use <ID>           # Switch current scene
vinput scene remove <ID>        # Remove scene
```

#### LLM Configuration

```bash
vinput llm list                 # List all providers
vinput llm add                  # Add provider (interactive)
vinput llm edit                 # Edit provider
vinput llm use <name>           # Switch current provider
vinput llm remove <name>        # Remove provider
vinput llm enable               # Enable LLM post-processing
vinput llm disable              # Disable LLM post-processing
```

#### Hotword Management

```bash
vinput hotword get              # View current hotword file path
vinput hotword set <path>       # Set hotword file
vinput hotword edit             # Open hotword file in editor
vinput hotword clear            # Clear hotword file configuration
```

#### Daemon Management

```bash
vinput daemon status            # Check daemon status
vinput daemon start             # Start daemon
vinput daemon stop              # Stop daemon
vinput daemon restart           # Restart daemon
vinput daemon logs              # View logs
```

## Scenes

Scenes control how LLM processes recognition results, switchable at runtime via scene menu key.

Each scene contains:

- **ID**: Unique identifier
- **Label**: Display name in menu
- **Prompt**: System prompt sent to LLM

The `default` scene calls LLM like other scenes (if enabled). To skip LLM, disable it globally or set scene candidate count to 0.

## Command Mode

Select text, press and hold command key, speak instruction, release to have LLM modify and replace selected content.

Examples:
- Select Chinese text → Press command key → Say "translate to English" → Release → Content replaced with English translation
- Select code → Say "add comments" → Release → Code replaced with commented version

Command mode requires LLM to be configured and enabled.

## LLM Configuration Example

Using local Ollama:

```bash
vinput llm add
# Fill in prompts:
# Name: ollama
# Base URL: http://127.0.0.1:11434/v1
# API Key: (leave empty)
# Model: qwen2.5:7b

vinput llm use ollama
vinput llm enable
```

## Configuration Files

| File | Path |
|------|------|
| Fcitx5 plugin config (keybindings, etc.) | `~/.config/fcitx5/conf/vinput.conf` |
| Core config (model, LLM, scenes) | `~/.config/vinput/config.json` |
| Model directory | `~/.local/share/fcitx5-vinput/models/` |

## Packaging and Release

Push a tag like `v0.1.0`, and GitHub Actions will automatically build and upload the following artifacts to Release:

- Source tarball `fcitx5-vinput-<version>.tar.gz`
- Ubuntu 24.04 `.deb`
- Arch Linux `.pkg.tar.zst`
