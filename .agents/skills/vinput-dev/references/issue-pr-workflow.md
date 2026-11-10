# Issue + PR Driven Development Workflow (SOP)

Development must follow a strict, chronological **Pre-Code Draft PR -> Single-Item Loop -> Merge** process for functional changes, with a fast-track policy for pure documentation and agent rules.

---

## 1. Scope Boundaries & Fast-Track Policy

To avoid process overhead on non-functional changes while strictly guarding runtime safety, repository changes are governed under two distinct tracks:

### Track A: Functional Code & Build (Strict Issue $\to$ PR Lifecycle)
- **Scope**: Any modifications touching `src/`, `CMakeLists.txt`, `flake.nix`, CI workflows (`.github/workflows/`), packaging manifests (AUR/Flatpak), or anything affecting compilation, binary artifacts, or daemon/addon runtime behaviors.
- **Mandatory Lifecycle**: Strict Issue $\to$ Pre-code Draft PR (all `- [ ]` unchecked) $\to$ single-item atomic commit & progressive push with PR checklist sync $\to$ `gh pr ready` for full remote CI validation $\to$ review bot triage $\to$ squash-merge.
- **Micro-slicing**: Keep functional code diffs under 300 lines; break large epics into stacked micro-PRs.
- **Pristine 3rdparty (零篡改铁律)**: Keep `3rdparty/` untouched upstream code. All wrappers and glue belong in `src/`.

### Track B: Text-Only, Documentation & Agent Governance (Direct Main Fast-Track)
- **Scope**: Pure documentation (`docs/`, `*.md`, `THIRD_PARTY_NOTICES.md`), agent skills, prompts, and rules (`AGENTS.md`, `CLAUDE.md`, `.agents/skills/`).
- **Policy**:
  1. **Zero Functional Drift**: Does not alter runtime code, binaries, or dependencies.
  2. **Exempt from Issue/PR**: No need to create a tracking Issue or open a Draft PR.
  3. **No Heavy CI**: Append `[skip ci]` in the commit message to prevent consuming unnecessary GitHub Actions runner hours.
  4. **Direct-to-Main Execution**: Run local pre-commit checks (`mise run check:changed`), commit directly, and push to `main`.

---

## 2. Planning & Research Model

To avoid confusing functional planning with toolchain validation:

1. **Phase 0: Upfront Capability & Prior-Art Grounding (Pre-design Research - BEFORE BRANCHING)**:
   - Discovering *how others solve it*: Inspect available search and retrieval tools (`deep_search_exa`, `google_search`, `context7`, `gh search`), benchmark mature open-source projects, analyze production post-mortems and official API docs, and report findings to the maintainer before opening Draft PRs.
2. **Phase A: Task Planning & Micro-Slicing (Pre-development - BEFORE CODING)**:
   - Defining *what* to build: Issue analysis, module boundaries, and opening the Draft PR with an unchecked `- [ ]` checklist (< 300 lines diff target).
3. **Phase B: Quality Gate & Opportunistic Clean-Up (Post-edit - AFTER EDIT)**:
   - Previewing steps via `mise run check:plan`, verifying with `mise run check:changed`, and opportunistically flattening deep `if-else` branching into guard clauses on touched files.

---

## 3. Chronological Lifecycle

```
+------------------------------------------------------------------------+
| 0. Phase 0: Upfront Grounding & Research (MANDATORY BEFORE BRANCHING)  |
|    Inspect available search tools (Exa, Google, Context7, gh)          |
|    gh search / Exa / Context7 prior-art & incident retrieval           |
|    Report evaluation & proposed design to maintainer in chat           |
+-----------------------------------+------------------------------------+
                                    |
+-----------------------------------v------------------------------------+
| 1. Pre-Code Initialization (MANDATORY BEFORE ANY CODE IS WRITTEN)       |
|    gh issue view <id>                                                  |
|    git checkout -b <type>/issue-<id>-<name>                            |
|    git commit --allow-empty -m "chore: initialize draft pr for #<id>"   |
|    git push -u origin <type>/issue-<id>-<name>                         |
|    gh pr create --draft (ALL tasks unchecked: - [ ])                   |
+-----------------------------------+------------------------------------+
                                    |
                +-------------------v-------------------+
                | 2. Single-Item Focused Development    |
                |    Only implement topmost - [ ]       |
                +-------------------+-------------------+
                                    |
                +-------------------v-------------------+
                | 3. Local Quality Gate & Pre-check     |
                |    mise run check:plan (preview steps)|
                |    mise run check:changed             |
                |    mise run fix (if needed)           |
                +-------------------+-------------------+
                                    |
                +-------------------v-------------------+
                | 4. Atomic Commit & Progressive Sync   |
                |    git add <files>                    |
                |    git commit -m "type(scope): ..."   |
                |    git push origin <branch>           |
                |    gh pr edit --body (check task N)   |
                |    (CI automatically skipped in Draft)|
                |    (Optional: gh workflow run ci.yml --ref <branch>) |
                +-------------------+-------------------+
                                    | (Remaining tasks?)
                                    +-------- Yes -------+
                                    | No                 |
+-----------------------------------v-------------------+|
| 5. Mark Ready, Merge & Close Issue                    ||
|    gh pr ready (activates full matrix CI & bots)      ||
|    gh pr checks (verify all CI checks pass)           ||
|    gh pr merge --squash --delete-branch               ||
|    git checkout main && git pull                      ||
|    gh issue view/close (guarantee closed state)       ||
+-------------------------------------------------------+|
                                    ^                    |
                                    +--------------------+
```

---

## 4. Detailed Execution Steps

### Phase 0: Upfront Capability & Prior-Art Grounding (BEFORE BRANCHING)
Before creating a branch or opening a Draft PR for any non-trivial issue, refactoring, or dependency proposal, the agent must perform upfront research:

1. **Capability Discovery**:
   Inspect available search tools in the current environment (`deep_search_exa`, `web_search_exa`, `google_search`, Context7 `/websites/...`, `gh` CLI).
2. **Three-Dimensional Grounding**:
   - **Code (`gh search`)**: Search how mature, high-trust open-source projects (Chromium, systemd, Fcitx5 addons, runner-images) solve similar challenges in production.
   - **Ecosystem & Incidents (Exa / Google)**: Search for production post-mortems, RFCs, and vetted Tier 2 Header-Only libraries (e.g., investigating GitHub runner apt mirror failover mechanisms).
   - **Official Docs (Context7)**: Retrieve primary upstream API specs and boundary guarantees.
3. **SSOT & Two-Tier Dependency Check**:
   Verify whether `src/common/` already provides canonical utilities, and ensure any third-party library strictly conforms to the Two-Tier and Pristine 3rdparty rules.
4. **Pre-Action Report to Maintainer**:
   Summarize open-source prior-art benchmarks, comparative trade-offs, and proposed implementation architecture in chat. Obtain confirmation before initializing the Draft PR.

### Phase 1: Pre-Code Initialization
```bash
# 1. Inspect issue
gh issue view <issue_id>

# 2. Create feature branch
git checkout -b feat/issue-<id>-<short-description>

# 3. Initialize branch with empty commit and push
git commit --allow-empty -m "chore: initialize draft pr for issue #<issue_id>"
git push -u origin feat/issue-<id>-<short-description>

# 4. Open Draft PR with ALL tasks UNCHECKED (- [ ])
gh pr create --draft \
  --title "<type>: <concise description> (#<issue_id>)" \
  --body "Closes #<issue_id>

### Implementation Tasks
- [ ] 1. Core data structures & config in src/common
- [ ] 2. Daemon audio stream handling in src/daemon
- [ ] 3. Update i18n & unit tests"
```

If the issue incompatibly changes `config.json` or `vinput.conf` keys, the checklist **must** include a task to add a `RegisteredSteps` entry in `src/common/config/config_migration.cpp`. Do not add runtime aliases; users migrate with `vinput config migrate`.

### Phase 2: Single-Item Execution Loop
For each unchecked `- [ ]` task in order:
1. **Code ONLY Task N**: Pick ONLY the topmost unchecked `- [ ]` item.
   - *Opportunistic Boy-Scout Clean-Up*: While editing touched files, flatten deep `if-else` branching ($\ge 4$ levels) into guard clauses (early returns) and eliminate avoidable nested loops to resolve `ast-grep` (`check:ast`) advisory warnings without expanding scope to untouched files.
2. **Quality Gate (CI-First)**:
   ```bash
   mise run check:plan
   mise run check:changed
   # For iterative diagnostics:
   hk run check --safe --format jsonl
   ```
   *Note*: Never run heavy local compilation (`mise run build-debug`). If the user is `xifan2333`, local compilation is **strictly prohibited**. Delegate compilation to GitHub Actions CI.
3. **Atomic Commit & Progressive Push**:
   Keep commits strictly atomic (one commit per `- [ ]` task) and push immediately:
   ```bash
   git add <modified_files>
   git commit -m "<type>(<scope>): complete task N (#<issue_id>)"
   git push origin <branch>
   ```
4. **Update PR Checklist in Lockstep**:
   Update the Draft PR body to check off `- [x]` for Task N immediately. This guarantees 100% synchronization between pushed commits and the visible PR checklist dashboard for full real-time traceability:
   ```bash
   gh pr edit --body "..."
   ```
   *Note on CI cost*: Full matrix CI and review bots are automatically skipped during the Draft phase, incurring **zero unnecessary runner minutes**.
5. **(Optional) On-Demand Remote Compilation Probe**:
   If you need to verify intermediate C++ compilation or Nix closure on remote CI before the PR is ready:
   ```bash
   gh workflow run ci.yml --ref <branch>
   ```

### Step 2.5: Code Duplication Triage & Human Decision Protocol (Anti-Slop Gate)
Before marking the PR ready or finalizing non-trivial implementations:
1. **Run Duplication Analysis**:
   ```bash
   mise run check:dup
   ```
2. **Dialectical Triage**:
   Analyze any newly introduced or touched clone blocks:
   - **Type A (Procedural Duplication / AI Slop)**: Unnecessary copy-pasting of helpers, parsing loops, or business logic. Must be abstracted and consolidated into canonical infrastructure under `src/common/`.
   - **Type B (Legitimate Domain Boilerplate)**: Inherent protocol structures, D-Bus XML mapping stubs, or serialization tables where abstraction harms readability or compiler optimization.
3. **Mandatory Pre-Action Report to Human (Awaiting Confirmation)**:
   The agent **MUST** output a structured report in chat before making refactoring edits or finalizing PR readiness:
   ```markdown
   ### Code Duplication Triage & Decision Report

   **1. Duplication Summary**
   - Clone locations: `<file_a>:<start_line>-<end_line>` and `<file_b>:<start_line>-<end_line>` (<lines> lines, <tokens> tokens)
   - Code pattern: ...

   **2. Dialectical Triage & Proposed Action**
   - **Type A (Procedural Duplication / Slop)**: ... (Explain why this should be abstracted into `src/common/...`)
   - **Type B (Legitimate Domain Boilerplate)**: ... (Technical rationale for retaining)

   **3. Awaiting Human Decision**
   Please confirm whether to proceed with the proposed refactoring or retention. Once confirmed, I will execute the decision.
   ```

### Phase 3: Mark Ready & Non-blocking Handover
Since every task commit was already pushed and checked off during Phase 2, mark the PR ready to activate full validation:
```bash
# 1. Mark PR ready for review (automatically triggers full matrix CI and review bots)
gh pr ready
```

**Strict Handover Rule (No Blocking Polling)**:
Agents are **strictly prohibited** from busy-waiting or executing blocking polling loops (e.g. `gh run watch`, `while true` sleep polling, or repeatedly spamming `gh pr checks`). Polling locks the conversation context, consumes unnecessary tokens, and is far less timely than GitHub's native push/web notifications.
Immediately after marking ready, the agent **MUST** output a structured handover summary and conclude its turn:
1. Provide the PR link and GitHub Actions run URL;
2. Summarize completed tasks and verification points;
3. Hand over control to the human maintainer. The maintainer will inspect CI asynchronously and prompt the agent if review bot triage or fixes are needed.

### Phase 4: Event-Driven Review Triage & Fix Loop (Dialectical Protocol)
Triggered **asynchronously on maintainer prompt** or when review bot feedback is posted:
```bash
# 1. Inspect review bot comments (top-level and line-level threads)
gh pr view --comments
gh api repos/:owner/:repo/pulls/<pr_id>/comments
```

#### Step 4.1: Critical Dialectical Analysis (Do NOT blindly follow bots)
Never treat bot suggestions or `> Prompt for AI Agents` blocks as unquestioned commands. Review bots frequently generate false positives (e.g. demanding mutex locks on lock-free SPSC queues, adding redundant defensive null-checks, or violating project C++20 conventions).
Classify every bot finding into:
1. **True Positives (Actionable Issues)**: Real bugs, memory leaks, unhandled boundary cases, or race conditions.
2. **False Positives (Rejections / Noise)**: Hallucinations, anti-patterns violating project architecture, or unnecessary defensive bloating.

#### Step 4.2: Mandatory Pre-Action Plan Report (BEFORE WRITING CODE)
Before writing or changing any code, the agent **MUST** report its analysis and action plan to the user in chat using the following structure:
```markdown
### Review Bot Dialectical Analysis & Action Plan Report

**1. Accepted Actionable Issues (True Positives)**
- [Bot Name] Suggestion: ...
  - Analysis & Fix Strategy: ...

**2. Rejected False Positives (Noise)**
- [Bot Name] Suggestion: ...
  - Rejection Rationale: ... (Based on C++20 standard, lock-free semantics, or architecture)

**3. Next Steps**
I will implement the fixes in Section 1, run local quality gates, and push the commit.
```

#### Step 4.3: Execute with Intent & Push
Only after the plan is communicated, implement the minimal fixes:
```bash
git add <modified_files>
git commit -m "fix(review): address review feedback (#<issue_id>)"
git push origin <branch>
```

**Proactive CI Quota Cancellation**:
Before pushing or if an intermediate fix is needed, cancel obsolete runs to protect runner quota (preserving current HEAD):
```bash
gh run list --branch $(git branch --show-current) --status in_progress --json databaseId,headSha --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" | xargs -r -n 1 gh run cancel
```

*AI Attribution*: Whenever an agent authors PR descriptions, automated review fixes, or substantive comments, append:
`*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*`

### Phase 5: Final Squash-Merge & Issue Lifecycle Closure
```bash
# 1. Verify all checks pass
gh pr checks

# 2. (Optional for core changes) Trigger remote matrix dry build against PR branch
gh workflow run release.yml --ref <branch_name>

# 3. Perform squash-merge and delete remote branch
gh pr merge <pr_id> --squash --delete-branch

# 4. Switch to main and sync latest commits locally
git checkout main && git pull

# 5. Verify & enforce Issue closure (guarantee complete lifecycle closure)
issue_state=$(gh issue view <issue_id> --json state -q .state)
if [ "${issue_state}" != "CLOSED" ]; then
  gh issue close <issue_id> --comment "Closed via PR #<pr_id>."
fi
```
