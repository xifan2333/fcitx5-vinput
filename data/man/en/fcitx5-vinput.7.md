---
title: FCITX5-VINPUT
section: 7
header: fcitx5-vinput Manual
footer: fcitx5-vinput
...

# NAME
fcitx5-vinput - Architecture overview and concepts of voice input for Fcitx5

# DESCRIPTION
**fcitx5-vinput** is an offline-capable and cloud-assisted voice input framework for Linux desktops running Fcitx5. It connects system audio capture, local or remote automatic speech recognition (ASR), and Large Language Model (LLM) post-processing into a seamless desktop typing experience.

# ARCHITECTURE

```
+-------------------------------------------------------------+
|                     Fcitx5 Input Framework                  |
|  +-------------------------------------------------------+  |
|  |             fcitx5-vinput addon (.so)                 |  |
|  |  - Global shortcuts & PTT key event interception      |  |
|  |  - UI notifications & candidate window presentation   |  |
|  +---------------------------+---------------------------+  |
+------------------------------|------------------------------+
                               | D-Bus (org.fcitx.Vinput)
                               v
+-------------------------------------------------------------+
|                       vinput-daemon                         |
|  +--------------------+  +-------------------------------+  |
|  | PipeWire Audio     |  | VAD (Voice Activity Detection)|  |
|  +--------------------+  +-------------------------------+  |
|                                                             |
|  +--------------------+  +-------------------------------+  |
|  | sherpa-onnx Engine |  | Cloud ASR Provider Scripts    |  |
|  | (Offline ASR)      |  | (Doubao, Bailian, OpenAI, ...) |  |
|  +--------------------+  +-------------------------------+  |
|                                                             |
|  +-------------------------------------------------------+  |
|  | LLM Post-Processing & Scene Rewriting                 |  |
|  | (OpenAI-compatible endpoints / local adapters)        |  |
|  +-------------------------------------------------------+  |
+-------------------------------------------------------------+
```

# DEFAULT KEYBINDINGS

Keybindings can be customized in Fcitx5 Configuration → Addons → Vinput:

**Alt_R** (Trigger Key)
:   Tap to toggle recording start/stop; hold to push-to-talk (release to finish).

**Control_R** (Command Key)
:   Hold after selecting text in any application to modify or rewrite the selection using voice instructions.

**Shift_R** (Command Palette Key)
:   Open the unified interactive command palette with registered quick commands (`/model` or `/m` for command models, `/asr` or `/a` for ASR, `/scene` or `/s` for scenes, `/proc` or `/p` for adapters) and instant global search.

**Page Up** / **Page Down**, **Up** / **Down**, **1**-**9**
:   Navigate and select candidate text.

# RECOGNITION PIPELINE

1. **Audio Capture**: Audio is captured from the selected PipeWire input node.
2. **VAD Trimming**: Leading and trailing silence is removed to minimize latency.
3. **ASR Inference**: Audio is transcribed by local sherpa-onnx models (offline) or external provider scripts (cloud).
4. **Scene & LLM Post-Processing**: If a non-raw scene is active, recognized text is formatted, corrected, or translated via an LLM.
5. **Text Insertion**: Candidates are presented in the Fcitx5 candidate window and committed into the active application.

# SEE ALSO
**vinput**(1), **vinput-daemon**(1), **vinput-gui**(1), **vinput-config**(5), **fcitx5**(1)
