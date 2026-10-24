# Domain Glossary

Shared vocabulary for `fcitx5-vinput`. Skills and agents must use these terms as defined here; when a
concept is missing, that is a signal to add it here rather than to invent a synonym.

---

## Process model

### Three-stream subprocess model

The single contract every helper process follows — ASR providers and LLM adapters alike. Each stream
has exactly one job:

| Stream | Role |
| :--- | :--- |
| `stdin` | Input: audio (raw PCM or JSONL frames), request text, control events |
| `stdout` | Result: recognized text, `partial` / `final` events, adapter output |
| `stderr` | User notification: the only channel out of the daemon's sandbox |

`stderr` is not a log sink. A helper runs inside the daemon's sandbox with no window and no reachable
journald for the user, so a line written there is the only way it can say anything to the user.

### Notification line

One line of `stderr` is one desktop notification, delivered as soon as it is flushed and independent
of the session's outcome. Two line shapes are accepted:

- `{"title": "...", "body": "..."}` — the helper owns both strings, including their translation.
- Any other non-empty line — taken verbatim as `body`; the host supplies the title.

The title and body wording belongs to the helper, which reads the inherited `LANG` / `LC_ALL` from
the daemon environment. The host never composes, translates, prefixes, or truncates that text.

### NotificationBridge

The host-side single consumer of helper `stderr` inside `vinput-daemon`. Splits lines, parses the two
notification shapes, collapses duplicates inside a short sliding window, and forwards `(title, body)`
directly to the desktop notification service (`org.freedesktop.Notifications`) over the user session D-Bus.
Both ASR sessions and adapter supervision feed the same bridge; there is no per-subsystem notification code.

### ProcessSession

`src/common/utils/process_session.{h,cpp}`. The streaming, long-lived child-process facade. A dedicated
pump thread owns the three pipes; callers only push stdin bytes and pop `ChildEvent`s. Termination is
process-group wide (`SIGTERM` → grace → `SIGKILL`), so grandchildren a helper forked cannot survive it.
Cancellation is an eventfd write, safe from any thread, and the destructor always reaps.

### Run

`src/common/utils/process_utils.{h,cpp}`. The one-shot child-process facade: feed stdin, collect
stdout/stderr, bounded by a deadline. Everything short-lived goes through it.

### TerminateGracefully

`src/common/utils/process_utils.{h,cpp}`. Stops a process this one did not spawn, identified by
`(pid, start_time)` and signalled through a pidfd when the kernel offers one, so a recycled pid can
never be hit. A direct child does not need it — its pid cannot be recycled before it is reaped.

---

## Speech recognition

### Streaming ASR provider

A long-lived duplex child process used while the user holds the push-to-talk key
(e.g. `provider.bailian.streaming`). Emits `partial` text live and exactly one `final` at teardown.
Its input is hot-path audio, so the daemon may hand it either raw PCM bytes or base64 JSONL frames,
whichever the upstream endpoint actually consumes.

### Batch ASR provider

A one-shot child process spawned after the user releases the key. Raw PCM in, final transcript out.
It has no structured `error` channel on `stdout`, which makes `stderr` its only way to report a
failure.

### Local ASR provider

In-process `sherpa-onnx` inference inside `vinput-daemon`. No child process and no network.

### Recognition session

One push-to-talk utterance. `AsyncRecognitionSession` owns the worker thread that serialises every
backend call; a backend session is never touched from two threads.

### Partial / final

`partial` is cumulative visible text used for live candidate display. `final` is the single committed
transcript per utterance and ends collection. Upstream mid-utterance completions must surface as
`partial`, never as an early `final`.

---

## Post-processing

### Scene

A configured transformation applied to the recognized text (translation, punctuation, formatting),
with its own prompt and adapter.

### LLM scene adapter

A local OpenAI-compatible proxy that makes a foreign protocol — Doubao IME, MTranServer, or plain
regex cleaning — look like `POST /v1/chat/completions`. It listens on loopback and is reached through
the same HTTP client as a cloud LLM, so post-processing has one code path. Its `stderr` follows the
same notification contract as an ASR provider's.

### Post-processor

`src/daemon/postprocess/post_processor.cpp`. Applies the active scene to the transcript over a
uniform OpenAI-compatible HTTP call, whether the endpoint is remote or a loopback adapter.

---

## Configuration

### ConfigMigration

`src/common/config/config_migration.cpp`. The only place user-facing configuration keys may change.
A breaking change ships with a versioned migration step in the same PR; there are no runtime
compatibility aliases. Schema changes bump MINOR.

---

## Audio

### Output ducker

Lowers system output volume through `wpctl` while recording and restores it afterwards, always under a
timeout so a hung WirePlumber cannot stall the worker.
