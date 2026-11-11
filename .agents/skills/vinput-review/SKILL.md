---
name: vinput-review
description: "Review diffs along dual axes (Spec + Standards) and triage bot comments. 触发：代码审查、双轴评审、审查机器人对质、/vinput-review。"
---

# Offline Dual-Axis Code Review & Bot Triage

Independent static diff review along two separate axes without local compilation, plus dialectical review bot triage:

- **Spec Axis**: Verify diff against originating issue requirements and scope boundaries.
- **Standards Axis**: Audit diff against `CODING_STANDARDS.md` and Fowler code smell heuristics.
- **Review Bot Triage**: Three-way classification (`[FIX] / [DISMISS] / [ASK]`) guarding against AI slop and bot hallucinations.

---

## 1. Dual-Axis Static Review (Pre-Ready Gate)

Run in two parallel, isolated read-only subagents.

### Step 1: Pin Diff & Merge Base
Capture the exact commit range against the merge base:
```bash
git diff $(git merge-base main HEAD)...HEAD
git log $(git merge-base main HEAD)..HEAD --oneline
```
*Completion criterion*: Confirmed non-empty diff and valid merge-base.

### Step 2: Spawn Parallel Subagents
Spawn both subagents concurrently with fresh context windows:

1. **Subagent A (Spec Verification)**:
   - *Injected Context*: Full diff command, commit list, originating Issue body or Draft PR checklist.
   - *Prompt*: "Report under 400 words: (a) explicit requirements missed or partial; (b) unrequested behavior or scope creep; (c) logic that appears implemented but deviates from specification. Quote the issue requirement and diff hunk."
2. **Subagent B (Standards & Architecture Audit)**:
   - *Injected Context*: Full diff command, commit list, `CODING_STANDARDS.md`.
   - *Prompt*: "Report under 400 words: (a) breaches of documented standards (Deep modules, Seam discipline, RT audio lock-free/zero-alloc, C-ABI RAII); (b) Fowler smells (Feature Envy, Primitive Obsession, Speculative Generality, Middle Man). Skip anything tooling enforces (formatting, AST rules, complexity <= 15). Quote diff hunks."

### Step 3: Aggregate Findings
Present reports side-by-side without merging or reranking:

```markdown
## Spec Audit
- [Pass | Concern]: <Finding with citation of diff hunk and issue requirement>

## Standards Audit
- [Hard Violation]: <Breach of CODING_STANDARDS.md with file:line>
- [Judgement Smell]: <Fowler smell or architectural observation>

## Verdict
- [Ready | Action Required]: <Single-line summary of blocking items, if any>
```

*Completion criterion*: Dual reports delivered with clear `Ready` or `Action Required` verdict.

---

## 2. Code Duplication Triage Protocol (S2.5 Anti-Slop Gate)

Before finalizing PR readiness, run duplication analysis:
```bash
mise run check:dup
```

When clones are detected in touched files:
1. **Type A (Procedural Duplication / AI Slop)**: Unnecessary copy-pasting of algorithms, string trimming, or process management. Must be consolidated into canonical infrastructure under `src/common/`.
2. **Type B (Legitimate Domain Boilerplate)**: Protocol definitions, D-Bus XML mapping tables, or serialization keys where artificial abstraction hurts clarity or compiler jump tables.
3. **Mandatory Pre-Action Report to Maintainer (Awaiting Confirmation)**:
   ```markdown
   ### Code Duplication Triage Report

   **1. Clone Locations**
   - `<file_a>:<start>-<end>` and `<file_b>:<start>-<end>` (<lines> lines)

   **2. Dialectical Classification & Proposal**
   - **Type A (Procedural Slop)**: Proposed abstraction in `src/common/...`
   - **Type B (Domain Boilerplate)**: Rationale for retaining

   **3. Awaiting Decision**
   Please confirm whether to abstract or retain. I will not modify code until confirmed.
   ```

*Completion criterion*: Maintainer decision received before touching code.

---

## 3. Review Bot Dialectical Triage Protocol (Three-Way Triage)

When CodeRabbit, Greptile, Copilot, or automated bots post comments:

```bash
gh pr view --comments
gh api repos/:owner/:repo/pulls/<pr_id>/comments
```

### Critical Posture & Hard Invariants
Evaluate bot suggestions dialectically. Bots frequently hallucinate:
- Urging mutex locks or memory allocations inside PipeWire real-time audio callbacks (`audio_capture.cpp`);
- Demanding modern C++ across static C-ABI trampolines (`principles/c-abi-boundaries.md`);
- Suggesting patches to vendored libraries under `3rdparty/` (violates Pristine 3rdparty rule);
- Adding redundant defensive `if (ptr != nullptr)` checks inside trusted domain boundaries.

### Three-Way Classification
Classify every bot finding into exactly one bucket:
1. **`[FIX]` (True Positive - Actionable Bug)**: Real memory leak, data race, undefined behavior, or logic flaw. Plan minimal fix conforming to `principles/laziness-protocol.md`.
2. **`[DISMISS]` (False Positive - Noise / Rejection)**: Hallucinations, anti-patterns violating project architecture, or unnecessary defensive bloat. Formulate concrete technical rebuttal citing C++20 standard, C-ABI constraints, or lock-free guarantees.
3. **`[ASK]` (Maintainer Decision)**: Product trade-offs, naming preferences, or ambiguous design choices where automated consensus does not exist.

### Pre-Action Triage Report
Deliver report before modifying code:

```markdown
### Review Bot Dialectical Triage Report

**1. Accepted Fixes [FIX]**
- [Bot Name] Suggestion: ...
  - Fix Strategy: ... (Conforms to CODING_STANDARDS.md)

**2. Dismissed False Positives [DISMISS]**
- [Bot Name] Suggestion: ...
  - Rejection Rationale: ... (Cites C-ABI, RT thread safety, or Pristine 3rdparty)

**3. Maintainer Decisions [ASK]**
- [Bot Name] Suggestion: ...
  - Trade-off Analysis: ...

I will implement the [FIX] items once confirmed.
```

*Completion criterion*: Triage report confirmed by maintainer before applying fixes.
