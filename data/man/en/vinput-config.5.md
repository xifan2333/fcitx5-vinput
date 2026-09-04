---
title: VINPUT-CONFIG
section: 5
header: fcitx5-vinput File Formats
footer: fcitx5-vinput
date: August 2024
...

# NAME
vinput-config - Configuration format and options for fcitx5-vinput

# DESCRIPTION
**fcitx5-vinput** uses two configuration files:

1. *~/.config/vinput/config.json* — Core configuration for audio capture, ASR engines, LLM providers, and postprocessing scenes.
2. *~/.config/fcitx5/conf/vinput.conf* — Fcitx5 addon configuration for keybindings and UI settings.

# CONFIG.JSON STRUCTURE

The core configuration file is JSON-formatted with four top-level sections: **global**, **asr**, **llm**, and **scenes**.

## GLOBAL SECTION
Controls runtime audio environment and capture settings.

```json
"global": {
  "default_language": "en",
  "capture_device": "default",
  "duck_output_while_recording": false,
  "duck_output_volume": 0.25
}
```

**default_language** (*string*)
:   Default UI/model language code (e.g. `"en"`, `"zh"`).

**capture_device** (*string*)
:   PipeWire capture node name, or `"default"`.

**duck_output_while_recording** (*boolean*)
:   Whether to lower system audio volume via WirePlumber while recording.

**duck_output_volume** (*float*, `0.0` - `1.0`)
:   Target volume fraction when ducking is active (`0.25` = 25% original volume).

## ASR SECTION
Configures offline and cloud speech recognition engines.

```json
"asr": {
  "active_provider": "sherpa-onnx",
  "normalize_audio": true,
  "input_gain": 1.0,
  "vad": {
    "enabled": true,
    "threshold": 0.5,
    "min_speech_duration": 0.15,
    "speech_pad_ms": 300
  },
  "providers": [ ... ]
}
```

**active_provider** (*string*)
:   ID of the active ASR provider. Use `"sherpa-onnx"` for the offline local engine.

**normalize_audio** (*boolean*)
:   Normalize input audio waveforms before recognition.

**input_gain** (*float*)
:   Software recording volume multiplier.

**vad.enabled** (*boolean*)
:   Enable Voice Activity Detection to drop leading and trailing silence.

## LLM SECTION
Defines OpenAI-compatible API endpoints.

```json
"llm": {
  "providers": [
    {
      "id": "groq",
      "base_url": "https://api.groq.com/openai/v1",
      "api_key": "YOUR_API_KEY",
      "extra_body": {}
    }
  ],
  "adapters": [
    {
      "id": "my-adapter",
      "command": "python3",
      "args": ["/path/to/entry.py"],
      "auto_start": true
    }
  ]
}
```

**adapters[].auto_start** (*boolean*)
:   Whether to automatically launch the adapter process when **vinput-daemon** starts.

## SCENES SECTION
Defines prompt templates and candidate counts for text rewriting.

```json
"scenes": {
  "active_scene": "__raw__",
  "definitions": [
    {
      "id": "__raw__",
      "count": 0
    },
    {
      "id": "default",
      "label": "Default Polish",
      "prompt": "Fix spelling and punctuation while keeping original meaning.",
      "provider_id": "groq",
      "model": "llama-3.3-70b-versatile",
      "context_lines": 3,
      "count": 5
    }
  ]
}
```

**active_scene** (*string*)
:   ID of the currently selected scene. `__raw__` skips LLM post-processing.

**context_lines** (*integer*)
:   Number of preceding input lines sent to the LLM as conversational context.

**count** (*integer*, `0` - `9`)
:   Maximum number of rewritten candidates requested from the LLM (set to `0` to disable LLM for this scene). Candidates are deduplicated with the raw ASR transcript. If only 1 distinct candidate remains, it commits directly to screen; if distinct choices exist, a candidate menu is shown with the primary LLM rewrite focused and the raw transcript preserved as option 1.

# VINPUT.CONF STRUCTURE (FCITX5 ADDON)

The Fcitx5 addon configuration file is stored in INI format at *~/.config/fcitx5/conf/vinput.conf*.

```ini
[Trigger]
TriggerMode=Both
TriggerKey=Alt_R
CommandKeys=Control_R
SceneMenuKey=Shift_R
AsrMenuKey=F8
PagePrevKeys=Page_Up,KP_Page_Up
PageNextKeys=Page_Down,KP_Page_Down
MaxStreamingDisplayWidth=60
```

**TriggerMode** (*enum: Tap, Hold, Both*)
:   Trigger activation mode. `Tap` toggles recording; `Hold` records while key is pressed; `Both` enables tap-to-toggle and hold-to-talk.

**TriggerKey** (*key list*)
:   Keys used to trigger voice recording (default: Right Alt).

**CommandKeys** (*key list*)
:   Keys used to record a voice command on selected text (default: Right Control).

**SceneMenuKey** (*key list*)
:   Keys used to open the postprocess scene selection menu (default: Right Shift).

**AsrMenuKey** (*key list*)
:   Keys used to open the ASR provider / model selection menu (default: F8).

**MaxStreamingDisplayWidth** (*integer*, `0` - `500`)
:   Maximum visual column width for live streaming recognition preview. Older text is folded into 'head...tail' to keep the preedit tooltip within bounds. Set to `0` to disable folding (default: `60`).

# FILES
*~/.config/vinput/config.json*
:   User core configuration file.

*~/.config/fcitx5/conf/vinput.conf*
:   User Fcitx5 addon configuration file.

*~/.var/app/org.fcitx.Fcitx5/config/vinput/config.json*
:   Configuration path when running inside Flatpak.

# SEE ALSO
**vinput**(1), **vinput-daemon**(1), **fcitx5**(1)
