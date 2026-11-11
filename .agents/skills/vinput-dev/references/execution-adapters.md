# Execution Adapters

Three capability contracts. Each names what is needed, then how the host
provides it, then the fallback when the host cannot.

Probe the host before dispatching:

```bash
command -v herdr >/dev/null && test "${HERDR_ENV:-}" = 1
```

## 1. Reconnaissance

**Contract** — read-only, wide search, compressed verdict under 300 words
covering: how it works, why it is shaped this way, which invariants to preserve,
where the key sites are.

With a subagent tool:

```javascript
subagent({
  agent: "scout",
  task: "Read-only reconnaissance for issue #<id>. Inspect call paths, prior commits, and upstream ABI constraints. Report in four parts: how it works, why it is shaped this way, invariants to preserve, key file:line sites. Under 300 words. Do not edit files."
})
```

Fallback: `read` and `git grep` in-session, then state the four-part verdict
before editing.

## 2. Dual Review

**Contract** — two reviewers, isolated contexts, one axis each, no reranking
across axes. A writer reviewing its own diff shares the writer's blind spots.

With a subagent tool, dispatch both in one call:

```javascript
subagent({
  workflow: "parallel",
  args: {
    tasks: [
      {
        agent: "reviewer",
        task: "Read `git diff <merge-base>...HEAD` against the specification in issue #<id>. Report missing requirements, scope creep, and behaviour that deviates from the specification. Quote the requirement and the hunk. Under 300 words."
      },
      {
        agent: "reviewer",
        task: "Read `git diff <merge-base>...HEAD` against CODING_STANDARDS.md. Report standard breaches and Fowler smells. Skip anything tooling already enforces. Quote the hunk. Under 300 words."
      }
    ]
  }
})
```

Fallback: two sequential passes over the same diff, one axis per pass, reported
under separate headings.

## 3. Terminal Isolation

**Contract** — a process that streams output for more than 30 seconds, or that
runs until interrupted, must not occupy the conversational terminal.

In Herdr:

```bash
herdr pane split --current --direction right --cwd "$PWD" --no-focus
```

The pane carries watchers and stream tails: `journalctl -f`, daemon monitoring,
test watchers, remote log follows. Editing, git, and local gates stay in the
calling pane. Close the pane when the process ends.

Fallback: redirect to a file and read it on demand.

```bash
journalctl -f -u vinput-daemon > /tmp/vinput-daemon.log 2>&1 &
```

## Dispatch Rules

1. Fresh context per dispatch. Never resume an earlier subagent session.
2. Pass only the four-tuple: role, scope, artifact pointers, output schema.
3. Never pass conversational history.
4. Keep every dispatched subagent read-only; the main agent holds the only
   branch write.
