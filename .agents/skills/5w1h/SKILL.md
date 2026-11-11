---
name: 5w1h
description: "Scenario-driven 5W1H triage and structured proposal engine for fcitx5-vinput. Adapts to 4 scenarios: Review/Bot Feedback, Bug/Crash Forensics, CI/Build Failure, or Feature/Refactor RFC. Use for /5w1h [review|bug|ci|rfc]."
disable-model-invocation: true
---

# Scenario-Driven 5W1H Triage & Design Engine

Analyze the preceding message, logs, review comments, errors, or feature ideas using a structured 5W1H framework.

> **Non-Negotiable Execution Gate**:
> **Strictly prohibit writing or modifying code before receiving explicit maintainer confirmation.** Deliver the structured 5W1H brief first, propose a minimal plan, and await human approval.

> **Language Adaptation**:
> Respond in the language used in the current conversation or user prompt (e.g. Chinese if the discussion is in Chinese, English if in English).

---

## Scenario Detection & Routing

Automatically infer the scenario from the conversation context, or accept an explicit argument (`/5w1h <scenario>`):

1. **Review Feedback (`review`)**: Input contains CodeRabbit, bot reviews, human PR review comments, or requested changes.
2. **Bug & Crash Forensics (`bug`)**: Input contains stack traces, SIGSEGV, PipeWire XRuns, audio dropouts, or runtime defects.
3. **CI & Build Failures (`ci`)**: Input contains compiler errors, Clang-Tidy violations, CMake build interruptions, or cross-distro failures.
4. **Feature & Refactoring RFC (`rfc`)**: Input contains design proposals, architectural decoupling, domain modeling, or new feature requests.

---

## Playbook 1: Review Feedback & Bot Triage

*Focus: Dialectical evaluation, identifying genuine issues, and rebuffing false positives.*

Do not accept review comments uncritically. Ground evaluations in `principles/c-abi-boundaries.md` and `principles/dependency-spec.md`:

- **What (Feedback Demand)**: Concise, objective summary of the reviewer's or bot's specific critique.
- **Where (Target Location)**: Exact source file paths, class/function names, and line numbers.
- **Why (Dialectical Analysis — Core Gate)**:
  - *Genuine Issue*: Highlights a real C++20 memory leak, UB, logic flaw, or performance hazard.
  - *False Positive*: Violates one of the 6 canonical C ABI boundaries (e.g. demanding modern C++ in PipeWire/sd-bus trampolines), breaches Pristine 3rdparty rules, or suggests shallow over-engineering. Provide concrete technical rationale.
- **How (Action Plan & Reply Text)**:
  - For genuine issues: Outline the minimal fix conforming to `CODING_STANDARDS.md`.
  - For false positives: Draft a polite, technically grounded rebuttal explanation for the review thread.

---

## Playbook 2: Bug & Crash Forensics

*Focus: Runtime timing, pre-condition state transitions, and single root-cause fix.*

- **What (Symptom & Artifacts)**: Objective capture of the crash signal, error code, log trace, or user-visible defect.
- **Where (Originating Scope)**: Specific module, function, and thread (especially noting if on the RT audio callback thread).
- **When (Timing & State Transition — Critical)**: The exact lifecycle transition triggering the fault (e.g. PTT key-down, ASR dynamic backend reload, session destruction).
- **Why (Root Cause Mechanism)**: Underlying technical failure (race condition, lock-free queue saturation, dangling reference, Linux PID recycling collision).
- **How (Root-Cause Remediation)**:
  - Apply `principles/laziness-protocol.md`: Fix once in shared infrastructure; never scatter defensive null-check patches across callers.
  - Define the shortest working diff and the verification method.

---

## Playbook 3: CI & Build Failure Triage

*Focus: Compiler diagnostics, cross-distro skew, and deterministic standard adherence.*

- **What (Diagnostic Message)**: Key compiler error lines, linker undefined references, or Clang-Tidy check IDs.
- **Where (Target & Environment)**: CMake target, source file, and the specific runner environment/architecture (Ubuntu x86_64, aarch64 ARM, Arch, Fedora).
- **Why (Root Cause of Skew)**: Why did it pass locally but fail on remote CI? (Missing `#include`, uninstalled host packages, `-Werror` trigger, GCC vs Clang standard library divergence).
- **How (Compliant Fix)**:
  - Zero suppression: Never silence diagnostics with `// NOLINT` or `-Wno-*`.
  - Propose standard C++20 fixes or CMake/Flake dependency declarations.

---

## Playbook 4: Feature & Refactor RFC Design

*Focus: Stakeholder blast radius, module depth, and architectural ROI.*

- **What (Objective Capability)**: Clear definition of the capability to add or the architectural debt to eliminate.
- **Where (Architectural Layer)**: Is this a generic, stateless primitive (`src/common/`) or a high-level business workflow (`src/cli/`, `src/daemon/`, `src/addon/`, `src/gui/`)?
- **Who (Impacted Stakeholders — Critical)**: End-users, Fcitx5 addon frontend, background daemon, or downstream packaging manifests?
- **Why (Architectural Motivation)**: Why is this change necessary? Why can existing infrastructure not solve it? What is the tradeoff?
- **How (Implementation Architecture)**:
  - Data structures first: settle types, ownership (RAII), and concurrency models.
  - Apply the Deletion Test to verify module depth.
  - Check configuration impact: if modifying schema, include `ConfigMigration` steps.

---

## Handover Contract

Every 5W1H delivery must end with an explicit confirmation request:
> **"Please review and confirm the 5W1H proposal above. I will not modify any code until receiving explicit confirmation."**
