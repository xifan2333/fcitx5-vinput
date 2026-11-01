# ASR Audio Ingress, Dual-Mode Stdin Streaming Contract, and Offline Capacity Boundary

vinput-daemon establishes a strict single-responsibility audio pipeline: DaemonRuntimeController
abdicates all raw PCM buffering and chunk-slicing logic. The daemon communicates with streaming
providers over stdin supporting both raw binary PCM and Base64 JSONL, eliminates artificial chunk-delaying
hacks, enforces a hard 20-second capacity boundary for local offline models with VAD segmentation,
and adheres to a zero-fallback architectural standard. Outside the designated PipeWire C ABI boundary
isolated within AudioCapture, all internal audio ingress and runtime modules must strictly conform to
modern C++20 standards.

## Context

Historically, audio data handling in `fcitx5-vinput` drifted significantly from the external standards
defined in `vinput-registry`, upstream `sherpa-onnx`, and modern C++ engineering guidelines:

1. **Controller Data Overreach**: `DaemonRuntimeController` directly managed five raw audio state
   variables (`startup_pcm_buffer_`, `current_recording_pcm_`, `pending_chunk_pcm_`, `current_sample_count_`,
   and `current_input_gain_`). It manually executed slicing loops (`kStreamingChunkSamples = 1600`)
   and gain calculations across six methods, causing severe concurrency lock contention, state drifting,
   and redundant double-buffering.
2. **Artificial Streaming Chunk Delay & Stdin Ambiguity**: While `vinput-registry` specifies that `stdin`
   supports both raw PCM bytes and Base64 JSONL, `command_streaming_backend` forced Base64 encoding for
   all streaming providers. Furthermore, it withheld each chunk until the *next* chunk arrived solely to
   synthesize a redundant `"commit": true` flag on the final slice, injecting an artificial 100ms latency
   and breaking providers (e.g. Doubao streaming) that disconnect immediately upon commit.
3. **Unbounded Offline Monolith & Reshape Crash (Issue #500)**: Local offline models (`sherpa-offline`,
   e.g. Zipformer) have a static ONNX self-attention tensor dimension limit (~20 seconds, upstream
   `k2-fsa/sherpa-onnx` issue #1519). The daemon erroneously treated push-to-talk utterances as unbounded
   monolith arrays, merely trimming outer silence via `VadTrimmer`. Utterances exceeding 20–50 seconds
   inevitably crashed ONNX Runtime with unrecoverable Reshape kernel errors (`input tensor cannot be reshaped`),
   losing user speech without readable diagnostics.
4. **Boundary Confusion (C ABI vs C++20)**: Audio capture and buffer pipelines previously intermingled
   C-style raw pointers and manual array management with high-level runtime orchestration instead of
   isolating third-party C library boundaries from modern C++ abstractions.

## Decision

1. **Dual-Mode `stdin` Streaming Pipeline (Raw PCM & Base64 JSONL)**:
   - Aligned with `vinput-registry` and the Three-Stream Subprocess Model in `GLOSSARY.md`, the streaming
     provider contract on `stdin` first-class supports both:
     a) **Raw Binary PCM Stream**: Direct uncompressed `S16_LE` audio bytes piped into `stdin` without
        JSON wrapping (enabled via provider descriptor / `VINPUT_ASR_BINARY_MODE`). Zero encoding overhead.
     b) **Structured JSONL Frames**: `{"type": "audio", "audio_base64": "..."}` for endpoints requiring JSON.
   - **Immediate Emission (No Chunk Delay)**: Audio slices must be flushed to the provider immediately upon
     arrival. The daemon shall never withhold a chunk to infer terminal status.
   - **Clean Teardown**: Utterance conclusion is signaled solely by `{"type": "finish"}` followed by closing
     `stdin` (or closing the binary stream). The artificial `commit: true` chunk-delaying hack is abolished.

2. **One-Shot Batch Provider Pipeline**:
   - `stdin`: Receives raw binary `S16_LE` PCM bytes directly (`sys.stdin.buffer.read()`).
   - `stdout`: Emits raw final transcript text directly without JSON framing.
   - `stderr`: Governed by ADR-0001 (JSONL desktop notification lines `{"title": ..., "body": ...}`).

3. **Offline Model Capacity Boundary & VAD Segmentation (Closing Issue #500)**:
   - **20-Second Hard Capacity Boundary**: In-process offline neural recognizers (Zipformer int8) enforce a
     hard limit of **20 seconds** (320,000 samples at 16 kHz) per continuous inference tensor.
   - **VAD-Driven Utterance Segmentation**: Long offline recordings must not accumulate into an unbounded
     single tensor. In accordance with the official `sherpa-onnx-vad-microphone-offline-asr.cc` paradigm,
     audio is segmented by streaming VAD using pause bounds (`min_silence_duration` >= 500ms) or forced
     speech bounds (`max_speech_duration` <= 20s). Each bounded segment is decoded independently and
     concatenated.
   - **Defensive Pre-Inference Guard & Actionable Error**: If continuous unsegmented speech exceeds the 20-second
     limit without pauses, the daemon must intercept before calling ONNX Runtime, preventing internal Reshape
     crashes. It must notify the user with an actionable message stating the recording duration, offline capacity
     limit, and recovery steps (segment recording or switch to streaming), logging raw exceptions to debug.

4. **C ABI Hard Boundary for PipeWire vs Modern C++ Everywhere Else**:
   - **Strict C ABI Isolation within AudioCapture**: `AudioCapture` acts as the dedicated RAII isolation facade.
     All PipeWire C APIs (`pw_thread_loop`, `pw_stream`, `spa_pod`, C callback function pointers with `void* userdata`
     trampolines) remain strictly locked inside `audio_capture.cpp`. The boundary communicates with downstream
     consumers exclusively via lock-free SPSC queues (`readerwriterqueue`). No C++ exceptions or polymorphic
     wrappers may cross into PipeWire callbacks.
   - **Modern C++ Everywhere Else**: All downstream ingress buffers, state machines, session managers,
     and provider backends must strictly adhere to modern C++20 standards (`std::span`, `std::unique_ptr`,
     `std::string_view`, `std::format`, ranges, RAII resource ownership, zero diagnostic suppressions).
   - **Open to Standard Header-Only Wrappers**: If a mature, zero-dependency, C++20-compatible header-only
     PipeWire stream capture wrapper emerges in the community, it may supersede `AudioCapture`'s internal
     implementation without altering downstream C++20 interfaces.

5. **Orchestrator De-datafication & Zero-Fallback Invariant**:
   - `DaemonRuntimeController` completely divests itself of all raw PCM vectors and slicing algorithms.
     Audio sample buffering, device-boundary gain scaling, startup pre-roll preservation, and streaming chunking
     are fully delegated to the standalone `AudioRecordingBuffer` deep module. The controller manages D-Bus RPCs,
     high-level status transitions (`Idle`, `Recording`, `Inferring`, `Postprocessing`), and session lifecycle,
     without touching raw audio buffers (session orchestration will be further decoupled into `SessionLifecycleManager`
     in Phase III).
   - **No Fallback Policy**: Internal architectural refactoring shall not introduce backward-compatibility
     aliases, double-buffering fallback layers, or multi-tier fallback chains. Clean, elegant, and unambiguous
     domain boundaries take absolute precedence over legacy workarounds.

## Consequences

- Completely eliminates the 100ms artificial latency in streaming providers.
- Eliminates the root cause of the ONNX Reshape crash for continuous recordings > 20s (resolving Issue #500).
- Eliminates ~200 lines of fragile vector management and double-buffering inside `DaemonRuntimeController`.
- Establishes a pristine, standardized interface between daemon audio capture and `vinput-registry` providers.
- Maintains strict C linkage at PipeWire kernel/library boundaries while guaranteeing modern C++20 safety everywhere else.

## Related Issues and Records

- **Issue #459**: refactor(architecture): 架构深化评审——拆解总控 God module 与收敛三处浅接口
- **Issue #500**: 明确离线模型的最长连续录音时长，并提高超限报错的可读性
- **Issue #458**: refactor(process): 统一子进程调用层——保留 header-only subprocess.hpp 并收敛超时/双向 IO/优雅终止
- **Issue #457**: refactor(style): C ABI 硬边界审计与 C++20 风格迁移规划
- **Issue #400**: fix(audio): 彻底解耦 PipeWire 采集线程与 ASR 推理/D-Bus IPC 以满足实时安全规范 (STD-AUDIO-01)
- **ADR-0001**: Direct Desktop Notifications and Error Classification Abolition
- **Ecosystem Specification**: `xifan2333/vinput-registry` (`provider-protocol.md`, `resource-schema.md`)
