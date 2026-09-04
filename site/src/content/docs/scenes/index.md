---
title: Scenes & LLM
description: Rewrite, polish, and translate recognition results with LLM.
---

## Concepts

Raw text from ASR may contain filler words, typos, or formatting issues. **Scenes** define a prompt that tells an **LLM** how to rewrite the text.

```
ASR raw text → [Scene prompt + LLM] → Rewritten text
```

Core relationships:

- A **scene** is a configuration: prompt (instruction) + bound LLM provider + model
- An **LLM provider** is an OpenAI-compatible API endpoint (e.g. Ollama, Groq, OpenAI)
- An **LLM adapter** is an optional local bridge process that wraps a non-standard local model into an OpenAI-compatible API for a provider to point to

In short: **the scene decides "how to rewrite", the LLM provider decides "who rewrites", the adapter solves "incompatible API"**.

When LLM rewriting is not needed, use the built-in `__raw__` scene — recognition results go directly to input.

## Scenes

### Concept

Each scene contains:

| Field | Description |
|-------|-------------|
| `id` | Unique identifier |
| `label` | Display name in the menu |
| `prompt` | System prompt sent to the LLM |
| `provider_id` | Bound LLM provider |
| `model` | Model name to use |
| `context_lines` | Number of previous text lines sent as context to the LLM |
| `llm_max_candidates` | Maximum rewritten candidates requested from LLM (0 to disable LLM) |

Rewritten results are deduplicated with the raw ASR transcript. If only 1 distinct text remains (no change), it commits directly to screen; if distinct candidates exist, a selection menu is shown (focused on the primary LLM rewrite, with the raw transcript preserved at option `1`).

LLM is only invoked when `provider_id` + `model` + `prompt` are all configured.

Built-in scenes:
- **`__raw__`** — Bypasses LLM, outputs raw recognition text
- **`__command__`** — Used by command mode (see below)

Switch scenes at runtime with `Shift_R`.

Corresponding config:

```json
{
  "scenes": {
    "active_scene": "__raw__",
    "definitions": [
      {
        "id": "__raw__",
        "llm_max_candidates": 0
      },
      {
        "id": "default",
        "label": "Default",
        "prompt": "Correct ASR errors, keep original meaning...",
        "provider_id": "groq",
        "model": "openai/gpt-oss-120b",
        "context_lines": 3,
        "llm_max_candidates": 5
      }
    ]
  }
}
```

### GUI

In Vinput GUI, manage scenes in the **LLM** tab: add, edit prompt, bind provider and model, and configure **Context Lines**.

`context_lines` controls how many previous text lines Vinput sends to the LLM as extra context before rewriting. This is useful for translation, continuation, and style-consistent polishing. Set it to `0` to disable extra context.

### CLI

```bash
vinput scene list               # List all scenes
vinput scene add --id <id>      # Add a scene
vinput scene edit <id>          # Edit a scene
vinput scene use <id>           # Switch active scene
vinput scene remove <id>        # Remove a scene
```

When adding a scene with LLM, `--provider`, `--model`, and `--prompt` must all be provided.

## LLM providers

### Concept

An LLM provider is an OpenAI-compatible API endpoint. Scenes reference it via `provider_id`.

You can configure multiple providers — different scenes can bind to different providers.

Corresponding config:

```json
{
  "llm": {
    "providers": [
      {
        "id": "groq",
        "base_url": "https://api.groq.com/openai/v1",
        "api_key": "your-api-key"
      }
    ]
  }
}
```

### Setup example

Using local [Ollama](https://ollama.com):

```bash
vinput llm add ollama --base-url http://127.0.0.1:11434/v1
vinput scene add --id polish \
  --label "Polish" \
  --provider ollama \
  --model qwen2.5:7b \
  --context-lines 3 \
  --prompt "Rewrite the recognized text into polished Chinese."
vinput scene use polish
```

### CLI

```bash
vinput llm list                         # List configured providers
vinput llm add <id> --base-url <url>    # Add
vinput llm edit <id> --base-url <url>   # Edit
vinput llm remove <id>                  # Remove
```

## LLM adapters

### Concept

If your local model does not provide an OpenAI-compatible API directly, install an **LLM adapter**. An adapter is a local process that wraps the model into an OpenAI-compatible API, then you point your LLM provider's `base_url` to it.

Corresponding config:

```json
{
  "llm": {
    "adapters": []
  }
}
```

### GUI

In Vinput GUI, go to **Resources → LLM Adapters** to browse and install.
Manage adapter start/stop and autostart with daemon on the **Control** page.

### CLI

```bash
vinput adapter ls               # List installed adapters with autostart policy
vinput adapter list -a          # List available remote adapters
vinput adapter ps               # View adapter processes and runtime PID status
vinput adapter status <id>      # Show detailed status for a specific adapter
vinput adapter add <id>         # Install an adapter
vinput adapter start <id>       # Start adapter process
vinput adapter stop <id>        # Stop adapter process
vinput adapter restart <id>     # Restart adapter process
vinput adapter enable <id>      # Enable autostart with daemon
vinput adapter disable <id>     # Disable autostart with daemon
```

## Command mode

### Concept

Command mode is a special scene usage: select existing text, then use a voice instruction to have LLM rewrite it.

Flow: select text → hold `Control_R` → speak your instruction → release → done.

Under the hood it uses the built-in `__command__` scene. The default prompt template receives the selected text and spoken instruction through `{{selected}}` and `{{asr}}`, wrapped in Vinput-scoped XML tags (`<vinput-selected>` and `<vinput-asr>`).

**Examples:**
- Select Chinese text → say *"translate to English"* → replaced with translation
- Select code → say *"add comments"* → replaced with commented version

If there is no surrounding-text selection, it falls back to primary-selection clipboard content.

> Command mode requires an LLM provider to be configured, with `provider_id` and `model` bound in the `__command__` scene.
