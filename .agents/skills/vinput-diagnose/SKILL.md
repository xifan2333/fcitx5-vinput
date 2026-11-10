---
name: vinput-diagnose
description: "Diagnose runtime defects in fcitx5-vinput: PipeWire audio capture drops (XRuns), swallowing initial syllables, D-Bus service failures, or memory leaks. Use when debugging audio stutter, stream termination, latency regressions, or daemon crashes."
---

# vinput-diagnose

Systematic runtime diagnosis for `fcitx5-vinput` audio streaming, PipeWire integration, and daemon lifecycle defects.

---

## 1. Fast Diagnostic Triage

### Check Service Status and Logs
```bash
# Check daemon unit status
systemctl --user status vinput-daemon.service

# View recent daemon logs with errors highlighted
journalctl --user -u vinput-daemon.service -e -n 100 --no-pager
```

### Inspect Live D-Bus Service
```bash
# Verify D-Bus service registration
busctl --user status org.fcitx.Vinput

# Monitor incoming/outgoing D-Bus RPC calls
busctl --user monitor org.fcitx.Vinput
```

---

## 2. Audio Capture & PipeWire Diagnostics

Refer to the detailed guide: [audio-debugging.md](../vinput-dev/references/audio-debugging.md).

### PipeWire Stream & Buffer Tracing
- **Enable PipeWire Verbose Debugging**:
  ```bash
  PIPEWIRE_DEBUG=3 /usr/bin/vinput-daemon --debug
  ```
- **Identify XRuns & Dropouts**: Look for `spa.alsa: overrun` or `pw.stream: underrun` in stderr.
- **Trace Lock-Free Queue Saturation**: Verify whether `readerwriterqueue` overflows during high CPU load.

### Swallowing Initial Syllables / First Word Drop
If voice input drops the beginning of spoken sentences:
1. Check pre-roll buffer sizing in `src/daemon/audio/audio_capture.cpp`.
2. Inspect VAD trigger threshold in `src/daemon/asr/vad_segmentation.cpp` (pre-speech padding frames).
3. Verify that the PipeWire stream transition from idle to recording does not drop buffered samples.

---

## 3. Investigation Protocol (pstack fix-root-causes)

1. **Reproduce First**: Reproduce the failure with deterministic inputs (e.g. mock PCM audio file or `vinput` CLI test).
2. **Instrument, Don't Guess**: Add `VLOG_DEBUG` traces around suspected state transitions.
3. **Trace to the Origin**: Follow the data flow back to where state first became invalid. A guard is only a fix if the state it handles is legitimate.
4. **Regression Verification**: Write a behavioral test or automated check proving the defect is resolved.
