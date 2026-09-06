---
title: VINPUT-GUI
section: 1
header: fcitx5-vinput User Manual
footer: fcitx5-vinput
...

# NAME
vinput-gui - Graphical configuration and resource management tool for fcitx5-vinput

# SYNOPSIS
**vinput-gui**

# DESCRIPTION
**vinput-gui** is the Qt-based desktop graphical interface for **fcitx5-vinput**.

It provides a user-friendly interface for:

- **Audio & Control**: Selecting PipeWire microphone capture devices, adjusting recording gain, toggling VAD (Voice Activity Detection), and managing daemon service state.
- **Resource Management**: Browsing, downloading, activating, and uninstalling offline ASR models (**sherpa-onnx**), cloud ASR provider scripts, and LLM adapter scripts from the online registry.
- **LLM & Scenes**: Configuring OpenAI-compatible LLM providers, writing custom prompt templates, setting context history lines, and managing postprocessing scenes.
- **Hotwords**: Managing custom vocabulary and domain-specific words for local recognition models.

# FILES
*~/.config/vinput/config.json*
:   Core daemon and recognition configuration.

*~/.config/fcitx5/conf/vinput.conf*
:   Fcitx5 input method addon configuration.

*/usr/share/applications/vinput-gui.desktop*
:   Desktop entry file.

# SEE ALSO
**vinput**(1), **vinput-daemon**(1), **vinput-config**(5), **fcitx5**(1)
