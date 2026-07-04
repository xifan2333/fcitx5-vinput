---
title: Settings
description: Capture device, gain, VAD, and other global settings.
---

## Concepts

Global settings control Vinput's base runtime environment, independent of ASR engine or scene selection.

Corresponding config:

```json
{
  "global": {
    "default_language": "en",
    "capture_device": "rnnoise_source",
    "duck_output_while_recording": false,
    "duck_output_volume": 0.25
  },
  "asr": {
    "normalize_audio": true,
    "input_gain": 5.0,
    "vad": {
      "enabled": true
    }
  }
}
```

## Capture device

Select the microphone input source. Vinput captures audio via PipeWire — this lists available capture devices on your system.

### GUI

In Vinput GUI, select the device on the **Control** page.

### CLI

```bash
vinput device list              # List available capture devices
vinput device use <name>        # Set active device
```

## Input gain

`input_gain` controls the recording volume multiplier. Increase this if your microphone is too quiet for good recognition.

```bash
vinput config set /asr/input_gain 5.0
```

## Audio normalization

`normalize_audio` normalizes the audio signal for more consistent volume levels.

```bash
vinput config set /asr/normalize_audio true
```

## VAD (Voice Activity Detection)

VAD detects whether someone is speaking. When enabled, silent segments are filtered out automatically, reducing unnecessary recognition attempts.

```bash
vinput config set /asr/vad/enabled true
```

## Reduce output volume while recording

When `duck_output_while_recording` is enabled, Vinput lowers the system output
volume while recording and restores it afterwards. This helps when you use
speakers and a microphone at the same time — for example listening to music
while dictating — so that speaker sound leaking into the microphone does not
degrade recognition.

`duck_output_volume` is the fraction of the current output volume kept while
recording (`0.25` keeps 25%). It is clamped to the `0.0`–`1.0` range. This
feature is off by default.

Volume is adjusted through WirePlumber (`wpctl`), so it matches the system
volume and is fully restored afterwards. If WirePlumber is not available (for
example inside some sandboxes), the feature is silently skipped.

```bash
vinput config set /global/duck_output_while_recording true
vinput config set /global/duck_output_volume 0.25
```

Both options are also available on the **Control** page in Vinput GUI, under the
**Audio** section.

## Language

`default_language` sets the default language, affecting UI display and some model behavior.

```bash
vinput config set /global/default_language zh
```

## General CLI

```bash
vinput config get <JSON Pointer>        # Read any config value
vinput config set <JSON Pointer> <val>  # Write any config value
vinput config edit core                 # Edit core config (config.json)
vinput config edit fcitx                # Edit Fcitx addon config (vinput.conf)
vinput daemon restart                   # Restart daemon to apply changes
```
