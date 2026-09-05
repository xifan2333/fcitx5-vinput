---
title: VINPUT
section: 1
header: fcitx5-vinput User Manual
footer: fcitx5-vinput
date: August 2024
...

# NAME
vinput - Command-line control and management tool for fcitx5-vinput

# SYNOPSIS
**vinput** [*GLOBAL OPTIONS*] *COMMAND* [*SUBCOMMAND*] [*OPTIONS*]

# DESCRIPTION
**vinput** is the command-line interface for **fcitx5-vinput**, an offline and cloud-assisted voice input addon for the Fcitx5 input method framework.

It allows users to manage offline speech recognition (ASR) models, cloud ASR provider scripts, LLM providers and adapters, recognition scenes, audio input devices, hotwords, daemon lifecycle, and recording state.

# GLOBAL OPTIONS
**-h**, **--help**
:   Print help message and exit.

**-v**, **--version**
:   Display program version and exit.

**-j**, **--json**
:   Output results in machine-readable JSON format.

# COMMANDS

## INITIALIZATION
**init** [**-f**, **--force**]
:   Initialize default configuration files and directories under *~/.config/vinput* and *~/.local/share/vinput*. If **-f** is specified, existing configuration will be overwritten.

## MODEL MANAGEMENT
Manage offline local speech recognition models powered by **sherpa-onnx** (not available in the Lite package).

**model list** [**-a**, **--available**]
:   List installed models. If **-a** or **--available** is passed, list models available in the remote registry.

**model add** *ID*
:   Download and install a model by its registry short *ID*.

**model use** *ID*
:   Set the specified installed model as the active local ASR model.

**model remove** *ID*
:   Uninstall the specified local model.

**model info** *ID*
:   Display detailed information and metadata for a model.

## ASR PROVIDER MANAGEMENT
Manage third-party cloud ASR providers (such as Doubao, Aliyun Bailian, ElevenLabs, OpenAI-compatible).

**provider list** [**-a**, **--available**]
:   List configured ASR providers. If **-a** is given, browse available provider scripts in the registry.

**provider add** *ID*
:   Install a provider script from the registry by *ID*.

**provider use** *ID*
:   Switch the active ASR provider to *ID*.

**provider edit** *ID*
:   Open the provider script or configuration in the system editor.

**provider remove** *ID*
:   Uninstall the specified provider script.

## LLM PROVIDER MANAGEMENT
Manage OpenAI-compatible LLM endpoints for scene post-processing and text rewriting.

**llm list**
:   List configured LLM providers.

**llm add** *ID* **-u**, **--base-url** *URL* [**-k**, **--api-key** *KEY*] [**-e**, **--extra-body** *JSON*]
:   Add a new LLM provider with base URL, optional API key, and optional extra JSON request body parameters.

**llm edit** *ID* [**-u**, **--base-url** *URL*] [**-k**, **--api-key** *KEY*] [**-e**, **--extra-body** *JSON*]
:   Update settings for an existing LLM provider.

**llm remove** *ID*
:   Remove an LLM provider.

**llm test** *ID*
:   Test network connectivity and API authentication for the specified LLM provider.

## LLM ADAPTER MANAGEMENT
Manage local bridge processes that adapt non-standard models into OpenAI-compatible endpoints.

**adapter list** [**-a**, **--available**]
:   List local adapters with autostart status, or available remote adapters with **-a**.

**adapter ps**
:   List adapter processes and runtime status (status, PID, autostart, and command).

**adapter status** *ID*
:   Show detailed status and configuration properties for an adapter.

**adapter add** *ID*
:   Install an adapter script by *ID*.

**adapter start** *ID*
:   Start the background adapter process.

**adapter stop** *ID*
:   Stop the running adapter process.

**adapter restart** *ID*
:   Restart the specified adapter process.

**adapter enable** *ID*
:   Enable the adapter to autostart when the daemon starts.

**adapter disable** *ID*
:   Disable the adapter from autostarting with the daemon.

## HOTWORD MANAGEMENT
Manage domain-specific vocabulary and custom dictionaries for local sherpa-onnx models (not available in the Lite package).

**hotword get**
:   Show the path of the currently configured hotwords file.

**hotword set** *PATH*
:   Set the path to a custom hotwords text file.

**hotword clear**
:   Clear the configured hotwords file path.

**hotword edit**
:   Open the hotword file in the default text editor.

## AUDIO DEVICE MANAGEMENT
**device list**
:   List available PipeWire audio capture devices.

**device use** *ID*
:   Set the active audio capture device by node name or *default*.

## SCENE MANAGEMENT
Scenes define prompt templates and bound LLM configurations used to rewrite raw ASR text.

**scene list**
:   List all defined recognition scenes.

**scene add** **--id** *ID* [**-l**, **--label** *LABEL*] [**-t**, **--prompt** *PROMPT*] [**-p**, **--provider** *PROVIDER_ID*] [**-m**, **--model** *MODEL*] [**-c**, **--count** *N*] [**--show-raw** [*BOOL*]]
:   Add a new scene definition. *N* is the maximum number of distinct rewrites requested from the LLM; `0` disables LLM processing for the scene. Pass **--show-raw false** to exclude the raw ASR transcript from candidates, allowing single-candidate rewrites to commit directly to screen.

**scene edit** *ID* [*OPTIONS*]
:   Modify an existing scene configuration.

**scene use** *ID*
:   Set the active recognition scene.

**scene remove** *ID*
:   Delete a scene definition (built-in scenes like `__raw__` cannot be deleted).

## CONFIGURATION MANAGEMENT
**config get** *POINTER*
:   Query a configuration value using a JSON Pointer path (e.g. `/global/capture_device` or `/asr/input_gain`).

**config set** *POINTER* [*VALUE*] [**-i**, **--stdin**]
:   Update a configuration value at the given JSON Pointer path.

**config edit** *TARGET*
:   Open the configuration file in `$EDITOR`. *TARGET* must be either **core** (*~/.config/vinput/config.json*) or **fcitx** (*~/.config/fcitx5/conf/vinput.conf*).

## DAEMON CONTROL
**daemon status**
:   Show the runtime status of **vinput-daemon** (PID, active model, D-Bus connection).

**daemon start**
:   Start the background daemon via systemd user service.

**daemon stop**
:   Stop the daemon.

**daemon restart**
:   Restart the daemon.

## RECORDING CONTROL
**recording start**
:   Start voice recording.

**recording stop** [**-s**, **--scene** *SCENE_ID*]
:   Stop voice recording, trigger speech recognition, and apply the specified scene.

**recording toggle** [**-s**, **--scene** *SCENE_ID*]
:   Toggle recording start/stop.

# FILES
*~/.config/vinput/config.json*
:   Core daemon, ASR, LLM, and scene configuration.

*~/.config/fcitx5/conf/vinput.conf*
:   Fcitx5 input method addon configuration (shortcuts, candidate count, UI options).

*~/.local/share/vinput/models/*
:   Installed offline sherpa-onnx models directory.

*~/.local/share/vinput/providers/*
:   Installed external ASR provider scripts.

*~/.local/share/vinput/adapters/*
:   Installed LLM adapter scripts.

# SEE ALSO
**vinput-daemon**(1), **vinput-config**(5), **fcitx5**(1), **systemctl**(1)
