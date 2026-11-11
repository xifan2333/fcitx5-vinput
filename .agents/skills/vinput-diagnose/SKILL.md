---
name: vinput-diagnose
description: "fcitx5-vinput 运行时排障与 GDB/逆向二进制取证引擎 (Runtime crash, deadlock, XRun, and ELF inspection via GDB, coredumpctl, strace, readelf, addr2line without local compilation). Use when diagnosing crashes, SIGSEGV, hangs, buffer drops, or daemon defects. Command: /vinput-diagnose."
---

# Runtime & Binary Forensics Engine

Diagnostic workflow for crashes, deadlocks, buffer dropouts (XRuns), audio truncation, and IPC failures without requiring local compilation.

---

## 1. Crash & Core Dump Triage

When a crash or abnormal termination occurs (`SIGSEGV`, `SIGABRT`, `SIGBUS`):

1. **Locate and inspect core dumps**:
   ```bash
   coredumpctl list vinput-daemon
   coredumpctl info vinput-daemon
   ```
2. **Extract non-interactive backtrace with GDB**:
   ```bash
   coredumpctl gdb --batch \
     -ex "set print pretty on" \
     -ex "thread apply all bt full" \
     vinput-daemon
   ```
3. **Identify thread topology**:
   - **Thread: PipeWire RT Callback**: Fault in real-time audio thread? Look for invalid buffer pointers, allocation during callback, or priority inversion.
   - **Thread: D-Bus Event Loop**: Fault in `org.fcitx.Vinput` dispatch? Check iterator invalidation, null message references, or unhandled exceptions.
   - **Thread: ASR Inference Worker**: Fault in Sherpa-ONNX or VAD? Check slice bounds, float conversion overflows, or tensor shape mismatches.

---

## 2. ELF Binary & Symbol Inspection

When investigating prebuilt binaries (from CI or release packages) without local debug builds:

1. **Resolve crash offset to source line**:
   ```bash
   addr2line -C -f -e /usr/bin/vinput-daemon <hex_offset>
   ```
2. **Inspect dynamic symbols & demangle names**:
   ```bash
   nm -C -D /usr/lib/fcitx5/fcitx5-vinput.so | grep -E "(Vinput|PipeWire)"
   ```
3. **Verify library dependencies and RPATH**:
   ```bash
   readelf -d /usr/bin/vinput-daemon | grep -E "(NEEDED|RPATH|RUNPATH)"
   ldd /usr/bin/vinput-daemon
   ```

---

## 3. Dynamic Tracing & IPC Inspection

When the daemon hangs, stalls, or drops IPC requests:

1. **Monitor live D-Bus traffic**:
   ```bash
   busctl --user monitor org.fcitx.Vinput
   ```
2. **Trace system calls and futex locks**:
   ```bash
   strace -f -p $(pgrep vinput-daemon) -e trace=network,ipc,futex,read,write -s 256
   ```

---

## 4. Real-Time Audio & PipeWire Tracing

When audio syllables are swallowed, audio crackles, or XRuns occur:

1. **Enable verbose PipeWire stream tracing**:
   ```bash
   PIPEWIRE_DEBUG=3 /usr/bin/vinput-daemon --debug
   ```
2. **Check for SPA buffer overruns/underruns**:
   Filter stderr for `spa.alsa: overrun` or `pw.stream: underrun`.
3. **Inspect SPSC queue saturation**:
   Read queue metrics via debug logs or CLI status:
   `DroppedChunks`, `DroppedSamples`.

---

## 5. Reporting

Synthesize findings into a concise diagnostic brief:
- **Crash Site / Bottleneck**: File, function, and instruction offset.
- **Thread Context**: Exact thread and state transition.
- **Root Cause**: Mechanism failure (dangling pointer, futex deadlock, buffer underrun).
- **Remediation**: Recommended fix conforming to `principles/laziness-protocol.md` and `CODING_STANDARDS.md`.
