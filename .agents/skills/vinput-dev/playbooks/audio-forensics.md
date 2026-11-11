# Playbook: Audio & Daemon Runtime Forensics

The canonical execution workflow for diagnosing PipeWire audio capture defects, buffer dropouts (XRuns), swallowing initial syllables, or daemon lifecycle crashes.

> **Execution Contract**:
> Copy these steps verbatim into your task checklist (Todo List). Follow the evidence; never apply speculative patches without a verified root cause.

---

## Steps

1. **System Service & Journal Triage**:
   - Inspect daemon systemd user service status:
     ```bash
     systemctl --user status vinput-daemon.service
     journalctl --user -u vinput-daemon.service -e -n 100 --no-pager
     ```
   - Check if the daemon crashed, was killed by SIGSEGV/SIGABRT, or exited cleanly.

2. **D-Bus IPC Inspection**:
   - Verify daemon D-Bus service registration:
     ```bash
     busctl --user status org.fcitx.Vinput
     ```
   - Monitor live method calls and property changes:
     ```bash
     busctl --user monitor org.fcitx.Vinput
     ```

3. **PipeWire Stream & Buffer Tracing**:
   - Run daemon with verbose PipeWire tracing:
     ```bash
     PIPEWIRE_DEBUG=3 /usr/bin/vinput-daemon --debug
     ```
   - Inspect stderr for SPA buffer overruns / underruns:
     `spa.alsa: overrun`, `pw.stream: underrun`.
   - Check whether `readerwriterqueue` in `src/daemon/audio/audio_capture.cpp` saturated.

4. **Speech Onset & VAD Padding Analysis**:
   If reported defect is "swallowing initial syllables" or "dropping first words":
   - Inspect pre-roll buffer capacity in `src/daemon/audio/audio_capture.cpp`.
   - Inspect VAD pre-speech padding frames in `src/daemon/asr/vad_segmentation.cpp`.
   - Verify that the stream state transition from idle to recording does not discard buffered PCM frames.

5. **5W1H Forensic Briefing (Before Code Changes)**:
   Deliver forensic analysis to the maintainer:
   - **What**: Exact failure observed from logs/runtime traces.
   - **Where**: File and function of origin.
   - **Why**: Root mechanism (e.g. priority inversion, buffer allocation timing, VAD threshold).
   - **How**: Minimal proposed fix conforming to `principles/laziness-protocol.md` and `CODING_STANDARDS.md`.

6. **Regression Verification**:
   - Verify that the original symptom is resolved with runtime evidence.
   - Proceed to `playbooks/feature-or-fix.md` to land the fix cleanly.
