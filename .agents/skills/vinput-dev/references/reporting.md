# Disclosure Templates

Two kinds of report. **Blocking** reports end the turn: the agent waits for the
maintainer. **Informative** reports stream and the agent continues.

Fill every field. Omit a field only when it does not apply, and say so.

## 1. Grounding Report (blocking, S0)

```markdown
## Grounding: #<issue_id> <title>

### Upstream Practice
- <project>: <how it solves this> (<link or file path>)

### Empirical Result
- Prototype: /tmp/vinput-proto-<slug>
- Measured: <command> -> <timing, allocation, or exit code>

### Proposed Design
<interface shape, one paragraph>

### Boundary Impact
- D-Bus: <none | signature>
- Config: <none | migration step>
- Real-time path: <none | what changes>

Awaiting confirmation before branching.
```

## 2. State Handshake (informative, S1)

```markdown
## Track Open: S1 -> S2

- Issue: #<issue_id> <title>
- Branch: <type>/issue-<id>-<slug>
- Draft PR: #<pr_id>, <n> tasks
- Role: Single Writer

Starting task 1.
```

## 3. Task Heartbeat (informative, S2)

```markdown
## Task <k>/<n> Done

- Checked: `[x] <k>. <task>`
- Commit: `<type>(<scope>): <summary> (#<issue_id>)` at `<sha>`
- Gate: `mise run check:changed` passed in <t>s
- PR: checklist updated

Next: `[ ] <k+1>. <task>`
```

## 4. Friction Alert (blocking, S2)

Raised on the second special-case branch, or on an interface assumption that the
code has disproved.

```markdown
## Friction: design assumption broken

- Blocked on: <what the code refused to do>
- Cost to force: <the workarounds it would need>
- Rolled back: `git reset` to `<sha>`
- Options: <redesign interface | return to S0 | split the task>

Which way?
```

## 5. Duplication Triage (blocking, S2.5)

```markdown
## Duplication: <n> clones in touched files

### Type A — procedural
- `<file_a>:<start>-<end>` and `<file_b>:<start>-<end>`, <lines> lines
- Proposed home: `src/common/<...>`

### Type B — domain boilerplate
- `<file>:<start>-<end>`
- Retain because <reason>

Extract A, retain B?
```

## 6. Handover (informative, then halt, S3)

```markdown
## Ready for CI

- Spec review: <pass | concerns, one line each>
- Standards review: <pass | concerns, one line each>
- PR: <url>
- CI run: <url>

Session halted, zero polling. Resume on webhook.
```

## 7. Closure (informative, S4)

```markdown
## Lifecycle Closed

- Merged: #<pr_id>
- Issue: #<issue_id>, state <CLOSED>
- Local: `main` synced at `<sha>`
- Unblocked next: #<issue_id> <title>
```
