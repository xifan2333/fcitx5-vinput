---
title: Registry
description: "vinput-registry contribution guide: models, providers, and adapters."
---

## Overview

[vinput-registry](https://github.com/xifan2333/vinput-registry) is a separate repository that hosts all downloadable resources for Vinput: local ASR models, cloud ASR provider scripts, and LLM adapter scripts. When you click **Download** or **Install** in Vinput GUI, or run `vinput model add` / `vinput provider add` / `vinput adapter add`, the resources are fetched from this registry.

```
vinput-registry/
├── registry/
│   ├── models.json          # Local ASR model index
│   ├── providers.json       # Cloud ASR provider index
│   └── adapters.json        # LLM adapter index
├── i18n/
│   ├── en_US.json           # English display text
│   └── zh_CN.json           # Chinese display text
└── resources/
    ├── providers/<folder>/<variant>/
    │   ├── entry.py
    │   └── README.md
    └── adapters/<folder>/<name>/
        ├── entry.py
        └── README.md
```

## Core rules

- All scripts must be **self-contained** — only use the standard library of your chosen runtime. No third-party dependencies, no `pip install`, no `npm install`. This ensures scripts work on any machine out of the box.
- Scripts can be written in any language. Python 3 is the recommended choice because it is available on virtually all Linux distributions. Node.js, Bash, or other runtimes are also acceptable as long as the stdlib-only rule is met.
- The `command` field in registry JSON works like a VS Code `tasks.json` entry: it specifies the interpreter, and `args` specifies the script path.
- Download URLs use array form for fallback.
- Each script resource must include an entry script and `README.md`.

## ID conventions

| Type | Pattern | Example |
|------|---------|---------|
| Model | `model.sherpa-onnx.<model-name>` | `model.sherpa-onnx.sense-voice-zh-en-ja-ko-yue-int8` |
| Provider | `provider.<folder>.<variant>` | `provider.bailian.streaming` |
| Adapter | `adapter.<folder>.<name>` | `adapter.mtranserver.proxy` |

- `id` is a **stable machine identifier** — never rename once published
- `short_id` is for human display in CLI/GUI/logs only
- i18n display text is stored as `<id>.title` and `<id>.description` in `i18n/*.json`

## ASR provider scripts

### Non-streaming (batch)

A non-streaming provider receives a complete audio recording and returns the transcript.

**Runtime contract:**

| Item | Spec |
|------|------|
| Command | `python3` |
| Input | Raw PCM `S16_LE`, mono, `16000 Hz` via **stdin** (binary) |
| Output | Final transcript text via **stdout** |
| Errors | Human-readable messages to **stderr** |
| Exit codes | `0` success, `1` runtime error, `2` usage error |
| Dependencies | Standard library only (no third-party packages) |

**File structure:**

```
resources/providers/<folder>/batch/
├── entry.py
└── README.md
```

### Streaming

A streaming provider recognizes audio in real time as it arrives.

**Runtime contract:**

| Item | Spec |
|------|------|
| Command | `python3` |
| Input | JSONL via **stdin** |
| Output | JSONL via **stdout** |
| Errors | **stderr** only |
| Exit codes | `0` success, `1` runtime error, `2` usage error |
| Dependencies | Standard library only (no third-party packages) |

**Input protocol (stdin):**

```json
{"type":"audio","audio_base64":"<base64 PCM S16_LE mono 16kHz>"}
{"type":"finish"}
{"type":"cancel"}
```

**Output protocol (stdout):**

```json
{"type":"session_started","session_id":"..."}
{"type":"partial","text":"current partial transcript"}
{"type":"final","text":"confirmed transcript","segment_final":true,"utterance_final":true}
{"type":"error","message":"..."}
{"type":"closed"}
```

Transcript semantics:
- `partial.text` — full user-visible transcript at the current moment
- `final.text` — full confirmed transcript at the current moment
- `segment_final` — optional boolean indicating a mid-session segment commit (does not end the utterance after `finish`)
- `utterance_final` — optional boolean indicating the final utterance transcript after `finish`
- The script is responsible for accumulating and deduplicating segments

**File structure:**

```
resources/providers/<folder>/streaming/
├── entry.py
└── README.md
```

### Environment variables

Provider env names use the `VINPUT_ASR_*` namespace. Shared variables:

| Variable | Purpose |
|----------|---------|
| `VINPUT_ASR_API_KEY` | Bearer-style API credential |
| `VINPUT_ASR_APP_ID` | App identifier |
| `VINPUT_ASR_ACCESS_TOKEN` | Token credential (non-API-key style) |
| `VINPUT_ASR_URL` | Full endpoint override |
| `VINPUT_ASR_BASE_URL` | Base endpoint (script constructs final path) |
| `VINPUT_ASR_MODEL` | Remote model identifier |
| `VINPUT_ASR_LANGUAGE` | Transcription language hint |
| `VINPUT_ASR_PROMPT` | Recognition prompt or bias text |
| `VINPUT_ASR_TIMEOUT` | Network timeout (seconds) |
| `VINPUT_ASR_FINISH_GRACE_SECS` | Extra wait after `finish` before closing |
| `VINPUT_ASR_ENABLE_VAD` | Enable server-side VAD |
| `VINPUT_ASR_VAD_THRESHOLD` | VAD sensitivity |
| `VINPUT_ASR_VAD_PREFIX_PADDING_MS` | Audio padding before speech |
| `VINPUT_ASR_VAD_SILENCE_DURATION_MS` | Silence duration to close turn |

Provider-specific variables are allowed when upstream features don't map to shared ones, but must still use the `VINPUT_ASR_*` prefix.

## LLM adapter scripts

An adapter is a one-shot command pipeline filter that rewrites recognized text or filters candidates using local scripts or models.

**Runtime contract:**

| Item | Spec |
|------|------|
| Command | Interpreter or executable (e.g. `python3`) |
| Execution | One-shot subprocess per rewrite request |
| `stdin` | Input text (raw string, or JSON in command mode) |
| `stdout` | Rewritten candidates (plain text or JSON) |
| `stderr` | User alerts forwarded to desktop notifications |
| Dependencies | Standard library only (no third-party packages) |

### Candidate output format (`stdout`)

Both standard scene rewriting and command mode share the same candidate extraction logic. An adapter can output candidates in any of the following formats:

1. **JSON format (recommended)**:
   A JSON object with a `candidates` array of strings (markdown code fences like ````json` are automatically stripped):

   ```json
   {
     "candidates": [
       "candidate 1",
       "candidate 2"
     ]
   }
   ```

2. **Multi-line plain text**:
   Each non-empty line is treated as a separate candidate.

3. **Single plain text**:
   The entire trimmed output is used directly as the single candidate.

### Input format (`stdin`)

- **Standard scene rewriting**:
  The raw recognized text string to be rewritten.

- **Command mode rewriting**:
  A JSON object containing both the voice instruction and the selected text:

  ```json
  {
    "text": "instruction from voice",
    "selected": "text selected in editor"
  }
  ```

**File structure:**

```
resources/adapters/<folder>/<name>/
├── entry.py
└── README.md
```

Adapter env names use a provider-specific prefix (e.g. `MTRAN_*`), not `VINPUT_ASR_*`.

## Local ASR models

Model entries in `models.json` describe downloadable sherpa-onnx model archives.

**Required fields:**

| Field | Description |
|-------|-------------|
| `id` | Stable ID, e.g. `model.sherpa-onnx.<name>` |
| `short_id` | Human-readable short ID |
| `urls` | Array of download URLs (fallback order) |
| `sha256` | Archive checksum |
| `size_bytes` | Archive size |
| `language` | `zh`, `en`, `multilingual`, etc. |
| `vinput_model` | Runtime metadata (see below) |

**`vinput_model` structure:**

| Field | Description |
|-------|-------------|
| `backend` | `sherpa-offline` or `sherpa-streaming` |
| `runtime` | `offline` or `online` |
| `family` | sherpa-onnx C API family: `dolphin`, `sense_voice`, `paraformer`, `transducer`, `qwen3_asr` |
| `language` | Language code |
| `size_bytes` | Model size |
| `supports_hotwords` | Whether hotword boosting is supported |
| `recognizer` | sherpa-onnx recognizer config (sample rate, decoding method, etc.) |
| `model` | sherpa-onnx model config (tokens file, family-specific model paths) |

Field naming follows sherpa-onnx C API conventions.

## Contributing a resource

### Adding a provider

1. Create `resources/providers/<folder>/<batch|streaming>/entry.py` + `README.md`
2. Add entry to `registry/providers.json`
3. Add i18n entries to `i18n/en_US.json` and `i18n/zh_CN.json`

### Adding an adapter

1. Create `resources/adapters/<folder>/<name>/entry.py` + `README.md`
2. Add entry to `registry/adapters.json`
3. Add i18n entries

### Adding a model

1. Add entry to `registry/models.json` with complete `vinput_model` metadata
2. Add i18n entries

### i18n entry format

```json
{
  "<resource-id>.title": "Display Name",
  "<resource-id>.description": "One-line description."
}
```

Both `en_US.json` and `zh_CN.json` must be updated.
