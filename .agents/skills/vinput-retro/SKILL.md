---
name: vinput-retro
description: "fcitx5-vinput 会话复盘与规则免疫自迭代引擎 (Retrospective engine to encode lessons into ast-grep rules, pointers, and standards refinement after CI runs or PR merges). Use after fixing CI failures, after PR merges, or during session retrospectives. Command: /vinput-retro."
---

# Retrospective & Self-Evolution Engine

Post-session retrospective engine to analyze defects, CI failures, or review findings, converting lessons into permanent engineering guardrails.

---

## 1. Review the Session or Failure

Identify the breakdown:
- What failed or required repeated correction? (Compiler error, CI matrix breakage, reviewer finding, runtime defect).
- What was the root cause of the miss? (Missing linter check, ambiguous navigation pointer, architectural judgement gap, or stale rule).

---

## 2. Four-Tier Enforcement Pipeline

Route the lesson strictly into one of the four categories:

### Tier 1: Structural Enforcement (Mechanical Rule)
*Principle: If a machine can check it, never write a natural language instruction.*
- If the fault is a fixed syntax pattern, banned API, missing header pattern, or unsafe call:
- **Action**: Author a new `ast-grep` rule under `.ast-grep/rules/<rule-name>.yml`, or add a script to `mise run check:changed`.
- **Constraint**: Do not add text warnings to `AGENTS.md` or `CODING_STANDARDS.md`.

### Tier 2: Navigation Refinement (Information Access)
- If the agent struggled to find a file or lacked context:
- **Action**: Add or sharpen a navigation pointer in `AGENTS.md`.

### Tier 3: Standards Calibration (Architectural Judgement)
- If the fault was an architectural misjudgement (shallow module, leaked C ABI resource, speculative interface for tests):
- **Action**: Update `CODING_STANDARDS.md` with a concise definition and an anti-pattern example.

### Tier 4: Pruning (Sediment Removal)
- Review existing instructions in `AGENTS.md` and `CODING_STANDARDS.md`:
- **Action**: If a rule is now enforced by `ast-grep` or is a no-op that modern models follow by default, delete it.

---

## 3. Proposal Delivery

Deliver a concise retrospective report:
- **Observed Defect**: The exact failure and why it occurred.
- **Classification**: Structural | Navigation | Standards | Pruning.
- **Concrete Diff**: Provide the YAML rule, pointer edit, or documentation diff for review before applying.
