# State, Role, and Access Boundaries

Each state assigns the agent a role. The role fixes what the agent may write.
Step actions and completion criteria live in `../SKILL.md`.

## Roles

| State | Role | May write |
| --- | --- | --- |
| S0 Explore | Scout | `/tmp/vinput-proto-*` only |
| S1 Slice | Planner | branch, commits, PR body |
| S2 Implement | Single Writer | feature branch |
| S2.5 Gate | Auditor | nothing |
| S3 Handover | Dispatcher | PR state only |
| S4 Closure | Merger | `main`, Issue state |

## S0 Explore — Scout

Repository reads freely. Every branch state stays untouched until the maintainer
confirms the design, so an unverified premise costs nothing to discard.

## S1 Slice — Planner

Code stays read-only. The write surface is the branch, the empty commit, and the
Draft PR body. Prefactor structural preparation into its own child Issue, so
each PR carries either structure or behaviour, never both.

## S2 Implement — Single Writer

The agent is the sole writer of the feature branch. Subagents dispatched here
stay read-only: reconnaissance returns findings, not edits.

## S2.5 Gate — Auditor

Code writes pause. The audit reads the diff and reports classification; the
maintainer decides.

## S3 Handover — Dispatcher

Write access ends. The dispatcher's remaining acts are the two reviews, the
ready flag, and the handover report. Self-review does not substitute for the two
isolated reviews: the writer shares the writer's blind spots.

## S4 Closure — Merger

Activation comes from an event, not from the calendar: a CI result, a bot
comment, or a maintainer prompt. Each event names its own role — forensics on a
failure, triage on a review comment, merge on a green run.

## Handoff

Every dispatch from any state follows the dispatch rules in
`execution-adapters.md`.
