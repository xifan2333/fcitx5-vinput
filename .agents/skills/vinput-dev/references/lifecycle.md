# Issue-to-PR Traceability Contract

Every functional change walks one traceable track from Issue to closed Issue.

## Anchor

- Work starts from an existing Issue: `gh issue view <id>`.
- No Issue exists: create one first, then start.
- Documentation and skill changes follow the same rule.

## Branch

`<type>/issue-<id>-<slug>`, for example `feat/issue-632-audio-buffer`.
`<type>` is `feat`, `fix`, `refactor`, `chore`, or `ci`.

## Draft PR

Open the PR as a draft, immediately after the empty commit, before any
functional code. Body shape:

```markdown
Closes #<issue_id>

### Tasks
- [ ] 1. <task>
- [ ] 2. <task>

*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <ver>.*
```

`Part of #<issue_id>` replaces `Closes` when the PR is one slice of an epic.

## Lockstep

One commit per checklist item. The commit message carries the Issue id; the
checkbox update lands in the same turn as the push.

```bash
git add <files>
git commit -m "<type>(<scope>): <task summary> (#<issue_id>)"
git push origin <branch>
gh pr edit <pr_id> --body "..."
```

Full matrix CI and review bots stay idle during the draft phase, spending no
runner minutes.

## Slice Size

Scope each PR around logical cohesion and a single responsibility rather than
an arbitrary line count. Split only when changes cross independent architectural
boundaries, or when separating steps makes review and rollback substantially
cleaner. Avoid artificial fragmentation solely to satisfy an arbitrary size limit.

## Quota

Before pushing a new HEAD, cancel stale in-progress runs on the branch, keeping
the runs for the current HEAD and every run on `main`.

```bash
gh run list --branch $(git branch --show-current) --status in_progress \
  --json databaseId,headSha \
  --jq ".[] | select(.headSha != \"$(git rev-parse HEAD)\") | .databaseId" \
  | xargs -r -n 1 gh run cancel
```

## Closure

A merge does not end the track. After `gh pr merge --squash --delete-branch`:

```bash
git checkout main && git pull
gh issue view <issue_id> --json state -q .state
```

State other than `CLOSED` gets closed explicitly:

```bash
gh issue close <issue_id> --comment "Closed via PR #<pr_id>."
```

A closed child Issue advances the parent epic to its next unblocked child.
