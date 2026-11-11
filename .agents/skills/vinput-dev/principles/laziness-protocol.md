# Laziness Protocol & Minimal Complete Change Principle

Authoritative engineering principle for `fcitx5-vinput`.

> *“The best code is the code you never wrote. Solve the whole problem with the least new code.”*
> — Ponytail / pstack Laziness Protocol

---

## 1. Core Rule: The Smallest Complete Change

Whenever designing, implementing, or reviewing code changes, take the first option that fully works:

1. **Does it need to exist?** Skip features, options, and speculative flexibility nobody asked for. Reject "for later" code.
2. **Reuse existing project infrastructure**: Use canonical utilities in `src/common/` (`glaze`, `spdlog`, `process_utils.h`, `mio`, `readerwriterqueue`). Never write ad-hoc parsers, string trimmers, or process wrappers.
3. **Use standard C++20**: If the language or standard library provides it, use it.
4. **Reuse vendored dependencies**: Never add a new 3rdparty library for functionality that takes a few lines to implement cleanly with existing tools.
5. **Prefer direct clarity**: Write the simplest code a reader understands at a glance.
6. **Deletion beats addition**: Eliminating dead code, unused parameters, and obsolete wrappers is strictly superior to adding layers. The shortest working diff wins.

---

## 2. Scope vs. Bloat

- **What Scope IS**: All places the change must reach to be complete and correct—callers, tests, fixtures, config migrations, exports, and edge-case error handling.
- **What Scope IS NOT**: Refactoring unrelated code, adding unrequested configuration toggles, building speculative abstraction layers, or reformatting untouched files.

---

## 3. Bug Fixes: Root-Cause Once

Before editing any bug fix:
1. Search all callers (`git grep` / `rg`) of the affected function or symbol.
2. Fix the root cause once in the shared core module.
3. Never plaster over defects by scattering defensive `if (ptr != nullptr)` checks across multiple caller sites when the invalid state is illegitimate.

---

## 4. Transparent Reporting Protocol

Every implementation reply and PR summary must end with 1–2 transparent lines answering:
- **What was skipped or left unverified** (e.g. untested distro channels, optional hardware features).
- **Known risks or trade-offs** the maintainer must know before merging.
