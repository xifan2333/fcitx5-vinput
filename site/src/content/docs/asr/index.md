---
title: ASR
description: "Speech recognition: local models, cloud providers, and hotwords."
---

## Concepts

ASR (Automatic Speech Recognition) converts speech into text — the first step in the voice input pipeline.

```
Microphone → [ASR] → Raw text → (optional) Scene + LLM rewriting → Final text
```

Vinput provides three complementary ASR mechanisms:

- **Local models** — Offline recognition powered by [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx). No network required, privacy-friendly, low latency.
- **Cloud providers** — Third-party ASR APIs (Doubao, Aliyun Bailian, ElevenLabs, OpenAI, etc.). Typically better accuracy, but requires network and API keys.
- **Hotwords** — Domain-specific vocabulary to improve recognition of proper nouns with local models (supported by some models).

Local models and cloud providers are **mutually exclusive** — switch between them at runtime from the command palette (`Shift_R` → `/asr`). Hotwords take effect when a local model is active.

## Local models

### Concept

A local model is a set of sherpa-onnx compatible model files that run entirely offline. Each model has its own language, type, and size. Only one can be active at a time.

Corresponding config:

```json
{
  "asr": {
    "providers": [
      {
        "id": "sherpa-onnx",
        "type": "local",
        "model": "model.sherpa-onnx.sense-voice-zh-en-ja-ko-yue-int8",
        "timeout_ms": 15000
      }
    ]
  }
}
```

### GUI

In Vinput GUI, go to **Resources → Models**:

- **Available models** list: click **Download** to install
- **Installed models** list: click **Use** to activate, **Remove** to uninstall

### CLI

```bash
vinput model list               # List installed models
vinput model list -a            # List available remote models
vinput model add <name>         # Download and install
vinput model use <name>         # Activate
vinput model remove <name>      # Uninstall
vinput model info <name>        # View details
```

## Cloud providers

### Concept

A cloud provider is an external script that receives an audio stream, calls a third-party ASR API, and returns recognized text. Each provider has its own environment variable config (API key, URL, etc.).

Providers come in two modes:
- **Non-streaming** — Sends audio after recording ends, waits for complete result
- **Streaming** — Recognizes in real time as you speak, returns intermediate results

Corresponding config:

```json
{
  "asr": {
    "active_provider": "provider.doubaoime.streaming",
    "providers": [
      {
        "id": "provider.bailian.streaming",
        "type": "command",
        "command": "python3",
        "args": ["~/.local/share/vinput/providers/bailian/streaming"],
        "env": {
          "VINPUT_ASR_API_KEY": "your-api-key",
          "VINPUT_ASR_MODEL": "qwen3-asr-flash-realtime"
        },
        "timeout_ms": 60000
      }
    ]
  }
}
```

### GUI

In Vinput GUI, go to **Resources → ASR Providers**:

- Click **Install** to download a provider script
- After installation, go to the **Control** page to select and edit provider environment variables (e.g. API key)

### CLI

```bash
vinput provider list -a        # List available remote providers
vinput provider add <id>       # Install
vinput provider use <id>       # Switch to this provider
vinput provider edit <id>      # Edit config (environment variables)
vinput provider remove <id>    # Uninstall
```

### Available providers

| Provider | Mode | Description |
|----------|------|-------------|
| Doubao (non-streaming) | Non-streaming | Doubao / Volcengine fast file recognition |
| ElevenLabs | Non-streaming / Streaming | ElevenLabs speech-to-text API |
| Aliyun Bailian | Non-streaming / Streaming | Qwen3-ASR via OpenAI-compatible / Realtime API |
| Doubao (streaming) | Streaming | Doubao ASR Realtime via AI Gateway |
| Doubao IME (streaming) | Streaming | Unofficial Doubao IME real-time protocol |
| OpenAI-compatible | Non-streaming / Streaming | OpenAI `/v1/audio/transcriptions` or Realtime WebSocket |

## Hotwords

### Concept

A hotword file is a text file with one term per line, used to boost recognition accuracy for specific vocabulary with local models. Typical use cases: names, brand names, technical terms.

```text
OpenAI
speech recognition:2.0
deep learning:3.5
```

For models supporting per-entry weights, append `:<weight>` directly to the term. Do not add spaces around the colon: use `term:2.0`, not `term :2.0` or `term: 2.0`. Models without per-entry weights ignore the weight suffix and use only the term.

Not all models support hotwords — the model list indicates support.

### GUI

In Vinput GUI, go to the **Hotwords** tab to edit.

### CLI

```bash
vinput hotword get              # View current hotword file path
vinput hotword set <path>       # Set hotword file
vinput hotword edit             # Open hotword file in editor
vinput hotword clear            # Clear hotword config
```
