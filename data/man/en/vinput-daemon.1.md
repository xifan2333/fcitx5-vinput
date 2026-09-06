---
title: VINPUT-DAEMON
section: 1
header: fcitx5-vinput User Manual
footer: fcitx5-vinput
...

# NAME
vinput-daemon - Background speech recognition and processing daemon for fcitx5-vinput

# SYNOPSIS
**vinput-daemon**

# DESCRIPTION
**vinput-daemon** is the background service component of **fcitx5-vinput**. It communicates with the Fcitx5 addon over D-Bus (`org.fcitx.Vinput`).

The daemon is responsible for:

- Capturing microphone audio via **PipeWire**.
- Offline speech-to-text recognition powered by **sherpa-onnx**.
- Forwarding streaming or batch audio to cloud ASR provider scripts.
- Voice Activity Detection (**VAD**) to filter silence.
- Post-processing and rewriting recognized text using OpenAI-compatible LLM APIs.
- Managing background LLM adapter processes.
- Dynamic system audio ducking via **WirePlumber** during recording.

# RUNTIME LIFECYCLE & SERVICE

**vinput-daemon** is designed to run as a systemd user service or start on demand via D-Bus activation.

Start daemon immediately:
:   `systemctl --user start vinput-daemon.service`

Enable daemon on user login:
:   `systemctl --user enable --now vinput-daemon.service`

View daemon logs:
:   `journalctl --user -u vinput-daemon.service -f`

# ENVIRONMENT VARIABLES

**VINPUT_CAPTURE_REUSE**
:   Controls whether to keep a connected but inactive PipeWire capture stream alive after recording ends (default: `1`). This warm-path mechanism significantly reduces latency on subsequent voice triggers. Set to `0` to destroy the capture stream immediately upon recording completion.

**VINPUT_CAPTURE_IDLE_DESTROY_MS**
:   Idle grace period in milliseconds before destroying an inactive reusable PipeWire stream (default: `15000`, 15 seconds).

**VINPUT_DEBUG**
:   Set to `1` to enable verbose debugging logs in stdout/journald.

**VINPUT_CONFIG_PATH**
:   Override the path to `config.json`.

# PRIVACY & AUDIO CAPTURE

When the capture warm path is active, **audio is never read or buffered into memory** while recording is inactive. After the idle grace period expires, the stream is fully closed and destroyed.

# FILES
*~/.config/vinput/config.json*
:   Primary daemon configuration file.

*/usr/share/systemd/user/vinput-daemon.service*
:   Systemd user unit definition.

*/usr/share/dbus-1/services/org.fcitx.Vinput.service*
:   D-Bus activation service file.

# SEE ALSO
**vinput**(1), **vinput-config**(5), **fcitx5**(1), **systemctl**(1), **pipewire**(1)
