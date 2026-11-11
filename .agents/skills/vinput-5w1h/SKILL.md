---
name: vinput-5w1h
description: "fcitx5-vinput 场景化 5W1H 需求推演与审查辩证对质 (Review/Bot Feedback, Bug Forensics, CI Failure, Feature RFC triage). Use when receiving complex issues, review bot comments, crashes, or before writing code. Command: /vinput-5w1h."
---

# Scenario-Driven 5W1H Triage & Design Engine

Analyze the preceding message, logs, review comments, errors, or feature ideas using a structured 5W1H framework.

> **Non-Negotiable Execution Gate**:
> Strictly prohibit writing or modifying code before receiving explicit maintainer confirmation. Deliver the structured 5W1H brief first, propose a minimal plan, and await human approval.

> **Language Adaptation**:
> Respond in the language used in the current conversation or user prompt (Chinese if the prompt is in Chinese, English if in English).

---

## Scenario Detection & Routing

Infer the scenario from context, or accept an explicit argument (`/vinput-5w1h <scenario>`):

1. **Review Feedback (`review`)**: CodeRabbit, bot comments, human PR reviews, or requested changes.
2. **Bug & Crash Forensics (`bug`)**: Stack traces, SIGSEGV, PipeWire XRuns, audio dropouts, or defects.
3. **CI & Build Failures (`ci`)**: Compiler errors, Clang-Tidy violations, CMake failures, or distro skew.
4. **Feature & Refactoring RFC (`rfc`)**: Design proposals, architectural decoupling, or feature requests.

---

## Playbook 1: Review Feedback & Bot Triage

Evaluate review comments dialectically. Ground analysis in `principles/c-abi-boundaries.md` and `principles/dependency-spec.md`:

- **What (Feedback Demand)**: Concise summary of the critique.
- **Where (Target Location)**: Target file paths, classes, and lines.
- **Why (Dialectical Analysis)**:
  - *Genuine Issue*: Real C++20 memory hazard, UB, logic flaw, or performance regression.
  - *False Positive*: Violates C ABI boundaries (such as demanding modern C++ in PipeWire/sd-bus callbacks), breaches Pristine 3rdparty rules, or suggests shallow over-engineering. Provide concrete technical rationale.
- **How (Action Plan & Reply Text)**:
  - For genuine issues: Outline minimal fix conforming to `CODING_STANDARDS.md`.
  - For false positives: Draft a polite, grounded rebuttal for the review thread.

---

## Playbook 2: Bug & Crash Forensics

- **What (Symptom & Artifacts)**: Crash signal, error code, log trace, or defect.
- **Where (Originating Scope)**: Specific module, function, and thread (noting if on RT audio callback thread).
- **When (Timing & State Transition)**: Exact lifecycle transition triggering the fault (such as PTT key-down, ASR backend reload, session destruction).
- **Why (Root Cause Mechanism)**: Underlying technical failure (race condition, queue saturation, dangling pointer, PID recycling collision).
- **How (Root-Cause Remediation)**:
  - Apply `principles/laziness-protocol.md`: Fix once in shared infrastructure; never scatter defensive null-check patches across callers.
  - Define the shortest working diff and verification method.

---

## Playbook 3: CI & Build Failure Triage

- **What (Diagnostic Message)**: Key compiler error lines, linker undefined references, or Clang-Tidy IDs.
- **Where (Target & Environment)**: CMake target, source file, runner architecture (Ubuntu x86_64, aarch64 ARM, Arch, Fedora).
- **Why (Root Cause of Skew)**: Reason it passed locally but failed on remote CI (missing include, uninstalled host package, -Werror trigger, GCC vs Clang standard library divergence).
- **How (Compliant Fix)**:
  - Zero suppression: Never silence diagnostics with `// NOLINT` or `-Wno-*`.
  - Propose standard C++20 fixes or CMake dependency declarations.

---

## Playbook 4: Feature & Refactor RFC Design

- **What (Objective Capability)**: Capability to add or architectural debt to eliminate.
- **Where (Architectural Layer)**: Stateless primitive (`src/common/`) or high-level orchestration (`src/cli/`, `src/daemon/`, `src/addon/`, `src/gui/`).
- **Who (Impacted Stakeholders)**: End-users, Fcitx5 addon frontend, daemon, or downstream packaging manifests.
- **Why (Architectural Motivation)**: Why necessary? Why existing infrastructure cannot solve it? Trade-offs?
- **How (Implementation Architecture)**:
  - Data structures first: settle types, ownership (RAII), and concurrency models.
  - Apply the Deletion Test to verify module depth.
  - Check configuration impact: if modifying schema, include `ConfigMigration` steps.

---

## Handover Contract

Every 5W1H delivery must end with an explicit confirmation request:
> **"Please review and confirm the 5W1H proposal above. I will not modify any code until receiving explicit confirmation."**
