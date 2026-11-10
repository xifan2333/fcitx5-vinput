---
name: vinput-correct
description: "Continuous governance engine for fcitx5-vinput. Convert human review corrections or recurring mistakes into structural defenses (architecture -> types -> ast-grep/linter -> tests -> docs last). Use when receiving review feedback or fixing repeated failure modes."
---

# vinput-correct

When the human maintainer or a review bot corrects a mistake in a PR, do not merely patch the single instance or append more text to `AGENTS.md`. Change the repository so the mistake cannot be made again.

> *“Assume every contributor is an agent that sees only the files it opened, copies the nearest example, and takes the shortest path that compiles. Design the repo so a change that looks right from one file is right for the whole repo.”* — pstack `/correct`

---

## 5-Level Defense Hierarchy

Fix each recurring mistake at the highest level that works:

### Level 1: Eliminate with Architecture
- Consolidate state to one owner; replace hand-synced lists with a single source of truth.
- Delete deprecated methods, dead code, and obsolete compatibility shims so an agent cannot copy them.
- Hide internal headers to prevent invalid cross-module imports.

### Level 2: Enforce with Types
- Make illegal states unrepresentable using C++20 `std::variant`, `std::optional`, or `expected<T, E>`.
- Use strong domain types (e.g. `SessionId`, `ChunkId`) instead of raw integers or strings.
- Pass required dependencies by explicit reference rather than nullable optional pointers.

### Level 3: Add an AST / Linter Rule
If bad code still compiles, add an automated rule whose error message explicitly names the alternative:
- **`ast-grep` Rule**: Add a rule under `.ast-grep/rules/<rule-name>.yml` with actionable message.
- **Git Hook / Script**: Add a deterministic Python check under `scripts/` wired into `hk.pkl` and `mise.toml`.

### Level 4: Test the Invariant
- Write a unit or integration test that fails when the invariant is broken.
- Ensure the test asserts observable behavior, not internal implementation details.

### Level 5: Update CODING_STANDARDS.md (Last Resort)
- Reserve documentation for genuine engineering judgement calls that cannot be automated.
- Nothing fails when an agent skips documentation; never rely on text rules for mechanical violations.

---

## Execution Loop

1. **Extract Mistake Class**: Identify the core pattern (e.g. ad-hoc JSON parsing, missing migration step, allocation in audio loop).
2. **Select Highest Level**: State why each higher level was unsuitable or choose the highest applicable defense.
3. **Implement Defense**: Write the AST rule, type constraint, or check script.
4. **Prove It Works**: Test the check against the past commit/diff to prove it catches the violation.
5. **Record in PR**: Document the new structural enforcement in the PR description.
