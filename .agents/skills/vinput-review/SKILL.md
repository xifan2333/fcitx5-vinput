---
name: vinput-review
description: "fcitx5-vinput 离线双轴独立审查技能 (Offline dual-axis static diff review: Spec fidelity vs CODING_STANDARDS and Fowler smells without local compilation). Use before marking PR ready, before pushing, or when reviewing git diffs. Command: /vinput-review."
---

# Offline Dual-Axis Code Review

Independent static diff review along two separate axes without requiring local compilation:

- **Spec Axis**: Does the diff faithfully implement the requirements without scope creep?
- **Standards Axis**: Does the code adhere to `CODING_STANDARDS.md` and Fowler code smell heuristics?

Run in a fresh, clean context to prevent implementation bias from polluting the review.

---

## 1. Diff Extraction

Capture the exact commit range against the merge base:

```bash
git diff $(git merge-base main HEAD)...HEAD
git log $(git merge-base main HEAD)..HEAD --oneline
```

Verify that the diff is non-empty before proceeding.

---

## 2. Axis 1: Spec Verification

Verify the implementation against the originating issue, draft PR checklist, or specification:

1. **Missing or partial requirements**: Did the author miss any explicit requirement from the issue?
2. **Scope creep**: Does the diff introduce unrequested abstractions, extra dependencies, or unsolicited refactoring?
3. **Behavioral correctness**: Does the logic correctly handle boundary states and failure paths?

---

## 3. Axis 2: Standards & Architecture Audit

Audit the diff against `CODING_STANDARDS.md`. Skip anything tooling already enforces (formatting, complexity $\le 15$, AST linter rules). Focus strictly on architectural judgement calls:

1. **Deep Modules**: Are modules deep, or are there shallow wrappers that merely forward calls?
2. **Seam Discipline**: Did the author introduce interfaces solely for unit test mocking? (Single-implementation interfaces are prohibited).
3. **Real-Time Audio Safety**: Does any code in PipeWire RT callback allocate memory or take blocking locks?
4. **C-ABI Boundaries**: Are native resources (PipeWire, sd-bus, POSIX fds) encapsulated in RAII wrappers immediately at the boundary?
5. **Boundary Discipline**: Is validation concentrated at system edges, with internal pure functions trusting domain types?
6. **Fowler Code Smells**: Check for Feature Envy, Primitive Obsession, Speculative Generality, Middle Man, and Data Clumps.

---

## 4. Output Format

Produce a structured report under 400 words:

```markdown
## Spec Audit
- [Pass | Concern]: <Finding with citation of diff hunk and issue requirement>

## Standards Audit
- [Hard Violation]: <Breach of CODING_STANDARDS.md with file:line>
- [Judgement Smell]: <Fowler smell or architectural observation>

## Verdict
- [Ready | Action Required]: <Single-line summary of blocking items, if any>
```
