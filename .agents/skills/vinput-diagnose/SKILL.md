---
name: vinput-diagnose
description: "Diagnose crashes, XRuns, GDB traces, and PipeWire faults without compilation. 触发：崩溃排障、断音丢帧、GDB取证、/vinput-diagnose。"
---

# Runtime & Binary Forensics Engine

Diagnostic workflow for crashes, deadlocks, buffer dropouts (XRuns), audio truncation, and IPC failures without local compilation.

---

## 1. Crash & Core Dump Triage

When a crash or abnormal termination occurs (`SIGSEGV`, `SIGABRT`, `SIGBUS`):

1. **Locate core dumps**:
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
3. **Map thread topology**:
   - **PipeWire RT Callback**: Look for buffer pointer invalidation, memory allocation during callback, or priority inversion.
   - **D-Bus Event Loop**: Check iterator invalidation, null message dereferences, or unhandled exceptions in `org.fcitx.Vinput`.
   - **ASR Inference Worker**: Check tensor shape mismatches, slice out-of-bounds, or float conversion overflows in Sherpa-ONNX/VAD.

*Completion criterion*: Exact crash thread identified with full stack trace and source line mapping.

---

## 2. ELF Binary & Symbol Inspection

When investigating prebuilt binaries (from CI or packaging) without local debug builds:

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

*Completion criterion*: Binary symbol, offset, and dependency chain verified against source.

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

*Completion criterion*: Stalled syscall or deadlock futex pinpointed to specific thread ID.

---

## 4. Real-Time Audio & PipeWire Tracing

When audio syllables are swallowed, audio crackles, or XRuns occur:

1. **Enable PipeWire stream tracing**:
   ```bash
   PIPEWIRE_DEBUG=3 /usr/bin/vinput-daemon --debug
   ```
2. **Check SPA buffer overruns/underruns**:
   Filter stderr for `spa.alsa: overrun` or `pw.stream: underrun`.
3. **Inspect SPSC queue saturation**:
   Read queue saturation metrics via debug logs or CLI status: `DroppedChunks`, `DroppedSamples`.

*Completion criterion*: Audio pipeline bottleneck classified (ALSA hardware underrun vs ring-buffer saturation).

---

## 5. Reporting

Synthesize findings into a concise diagnostic brief:
- **Crash Site / Bottleneck**: File, function, and instruction offset.
- **Thread Context**: Exact thread and state transition.
- **Root Cause Mechanism**: Dangling pointer, priority inversion, or buffer saturation.
- **Remediation**: Smallest fix conforming to `principles/laziness-protocol.md` and `CODING_STANDARDS.md`.
