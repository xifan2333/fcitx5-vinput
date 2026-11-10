---
name: vinput-review
description: "Two-axis code review for fcitx5-vinput along Standards (CODING_STANDARDS.md + Fowler smells) and Spec (originating issue/spec requirements). Use before marking a PR ready, when conducting self-reviews, or when requesting a diff evaluation."
---

# vinput-review

Two-axis review of the changes between `origin/main` (or a specified base) and `HEAD`:

- **Standards Axis**: Does the code conform to [`CODING_STANDARDS.md`](../../CODING_STANDARDS.md) and avoid Fowler code smells?
- **Spec Axis**: Does the code faithfully fulfill the originating issue/spec without missing requirements or introducing scope creep?

Both axes are deliberately evaluated separately so one does not mask the other.

---

## Process

### 1. Pin the Fixed Point
Determine the base ref (default: `origin/main`):
```bash
git diff origin/main...HEAD
git log origin/main..HEAD --oneline
```
Ensure the diff is non-empty before proceeding.

### 2. Identify Spec Source
Fetch the originating GitHub issue or PR description:
```bash
gh issue view <issue-number> --json title,body
# Or read the Draft PR body:
gh pr view <pr-number> --json title,body
```

### 3. Load Standards Source
Read [`CODING_STANDARDS.md`](../../CODING_STANDARDS.md) for project rules:
- **Audio RT Safety**: No allocations or blocking locks in audio callbacks.
- **Deep Modules**: No shallow wrappers; high leverage behind simple interfaces.
- **SSOT Infrastructure**: Reusing `glaze`, `spdlog`, `process_utils.h`, `mio`, `readerwriterqueue`.
- **Complexity**: Cognitive complexity $\le 15$; guard clauses; no nested dynamic loops.
- **Pristine 3rdparty**: Zero edits under `3rdparty/`.

Combine with the Fowler Code Smell baseline:
- *Mysterious Name*: Types or functions whose names do not reveal intent.
- *Duplicated Code*: Identical logic structures across files.
- *Feature Envy*: A function reaching into another module's fields more than its own.
- *Primitive Obsession*: Raw strings or primitives used where domain types belong.
- *Speculative Generality*: Unused abstractions or hooks added for speculative needs.
- *Shotgun Surgery*: A single conceptual change requiring edits scattered across many files.

### 4. Evaluate Both Axes

#### Standards Axis Findings:
- Report every place the diff violates `CODING_STANDARDS.md` (cite section and rule).
- Report any Fowler code smell heuristics detected in the diff.
- Note whether mechanical checks (`ast-grep`, `lizard`, `jscpd`) pass cleanly.

#### Spec Axis Findings:
- Requirements requested by the issue that are missing or incomplete.
- Unrequested behavior in the diff (scope creep).
- Requirements that look implemented but fail on edge cases.

### 5. Aggregate Side-by-Side
Present findings under separate headings:
```markdown
## Standards Review
- [Pass / Violations found]

## Spec Review
- [Pass / Scope creep / Missing items]

## Recommendation
- Ready for PR Ready / Requires changes before ready
```
