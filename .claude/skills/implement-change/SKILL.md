---
name: implement-change
description: Takes a change from a one-line prompt all the way to merged. Creates the branch, implements the change by delegating to an autonomous execution skill, opens a draft PR, waits for CI, marks it ready, handles Claude review feedback, and confirms the squash-merge lands. Use when the user invokes it with the work as an argument, or says "implement and ship X", "build X and get it merged", "take X all the way", or otherwise asks for a change carried to main without further input.
compatibility: Requires git and the gh CLI, both installed and authenticated.
---

# Implement Change

One prompt in, a merged commit out. This skill owns the entire lifecycle of a change: branch, build, PR, CI, review, merge, cleanup. The user is not expected to intervene at any point.

It joins two halves that are usually separate, and the seam matters. The *building* is delegated. The *shipping* is done here step by step, because it is mechanical and its failure modes are known. Do not let the delegated run take over the shipping half.

## When to invoke

- The user invokes it directly with the work as the argument.
- The user asks for a change and hands-off handling in one breath: "implement X and ship it", "build X and get it merged", "make X happen end to end".
- Do **not** invoke it merely because a change was requested. Ordinary requests are answered by doing the work. This is for when the user has signalled they want the change carried all the way to `main` without further input.

If the code is **already written and committed**, skip Step 2 and start at Step 3. Everything else applies unchanged.

## Delegate the implementation

Step 2 is the bulk of the work and should not be hand-rolled inline. Hand it to an autonomous execution skill, then come back here for the shipping half.

**Default: `autopilot`.** It runs the full phase sequence (requirements, design, implementation, QA, validation) without check-ins, which is what this step wants.

If it is not installed, substitute the closest equivalent available: any skill whose description covers carrying a task from requirements through implementation and verification autonomously. `ralph` (loop until complete), `ultrawork` (parallel execution) and `team` (coordinated agents) fill the same role, as does a built-in long-running mode such as `goal` if the installed Claude Code has one. Check what is actually present rather than assuming. If nothing of the kind exists, implement it directly, still working in phases: understand, locate, plan, implement, verify.

Two rules when delegating, both of which matter more than the choice of skill:

- **Pass the user's prompt through verbatim.** Do not summarize or narrow it. If genuine ambiguity blocks the work, ask the user rather than guessing.
- **Scope the delegation to the code.** State explicitly that the run must stop once the change is implemented, verified and committed on the branch, and must not push, open a PR, or merge. This skill owns those. If the delegated skill has a cancel or exit mode (`/oh-my-claudecode:cancel` for OMC modes), invoke it when the run finishes so its state does not leak into the steps below.

Do not restate general coding or testing standards to the delegated run. It can already code and can read `CLAUDE.md`, which carries this repo's conventions. Give it the prompt and the repo-specific traps in Step 2, nothing more.

## Workflow steps

Copy this checklist and check items off as you go:

```
- [ ] Step 1: Branch created off origin/main
- [ ] Step 2: Change implemented, verified, committed
- [ ] Step 3: Draft PR opened
- [ ] Step 4: CI green on the draft
- [ ] Step 5: Marked ready for review
- [ ] Step 6: `review` check green
- [ ] Step 7: Merge confirmed
- [ ] Step 8: Branch cleaned up
- [ ] Step 9: Reported to the user
```

### Step 1 — Create the branch, before any code is written

Do this first, not as cleanup afterwards. Every commit then lands where it belongs from the start, and nothing has to be moved off `main` later.

```bash
git fetch origin main
git checkout -b <branch-name> origin/main
```

Derive the branch name from the user's prompt: short, `kebab-case`, descriptive of the change (`add-textarea-widget`, `fix-touch-axis-transpose`). Never open a PR from `main`.

If the user is already on a feature branch created for this work, stay on it.

### Step 2 — Implement the change based on the prompt: $ARGUMENTS

Delegate this per the section above, then confirm the result before moving on. The delegated run is responsible for the change *and* for verifying it; this step is not complete just because files changed.

Before leaving this step, check the things this repo will otherwise fail on later:

- **Regenerate if you touched JSX.** `app/apps/*.js`, `app/app.js` and `app/ui-selftest.js` are generated from `app/src/*.jsx` by `node tools/build-app.mjs` and committed. A source edit without a rebuild fails CI, and a hand-edit to a generated file is a bug even when CI passes.
- **New binding calls.** `node tools/check-js-api.mjs` catches a script calling a binding the C layer does not register, or a widget method on the wrong kind of widget. Cheaper to run now than to read out of a CI log.
- **Firmware changes are not verified by CI.** `selftest.js` and `ui-selftest.js` run on the panel over serial, so no hosted runner executes them. If the change touches `firmware/`, note plainly whether it was flashed to a board or only compiled. Carry that note into the PR description and the final report. Never imply hardware verification that did not happen.
- **Everything is committed**, with no stray noise (`.temp/`, build output).

### Step 3 — Open the PR as a draft

Follow the `create-pull-request` skill (`.claude/skills/create-pull-request/SKILL.md`) in full for the push, diff review, title and body. Capture the PR number and URL.

Open it as a draft (`gh pr create --draft ...`). This is load-bearing rather than cosmetic: while the PR is a draft the review job skips, which also skips the job that arms auto-merge, so nothing can review or merge work that is still settling. Marking it ready later is the deliberate act that starts the gate.

### Step 4 — Wait for CI to pass on the draft

`ci.yml` runs on every push regardless of draft state. Poll until the checks on the current head SHA complete:

```bash
gh pr checks <PR> --watch
```

Expect `scripts` and `firmware`. The `firmware` job compiles the ESP32 toolchain and is the slow one; several minutes is normal, more on a cold cache.

If a check **fails**, read its log:

```bash
gh run view <run-id> --log-failed
```

Common failures here and what they actually mean:
- `build-app.mjs --check` failing means a generated app is stale. Run `node tools/build-app.mjs` and commit the output. If it reports all outputs stale on Windows, suspect line endings before suspecting the build.
- `check-js-api.mjs` failing means a script calls a binding the C layer does not register, or calls a widget method on the wrong kind of widget. That is a real bug that would otherwise surface only on the device.
- The doc link check failing means a relative link in a `.md` file points at a file that does not exist.

Fix, commit, push. CI re-runs automatically. Repeat until green.

If the failure is clearly infrastructure noise (runner flake, network timeout) and the same check passed on a prior run, re-run it:

```bash
gh run rerun <run-id> --failed
```

Do not re-run speculatively. Only when there is clear evidence of a transient failure.

### Step 5 — Mark the PR ready for review

Once CI is green:

```bash
gh pr ready <PR>
```

This fires `ready_for_review`, which triggers the review. Do not mark ready before CI is green: it costs a review of code that is about to change, and every push to a ready PR triggers another one.

### Step 6 — Wait for the Claude review

The verdict is the `review` check's conclusion, so watch the check rather than the comment:

```bash
gh pr checks <PR> --watch
```

The check is authoritative and the comment is the explanation. The summary comment is **sticky**, edited in place rather than reposted, so "the comment looks new" is not a signal that a re-review happened. If you need to know which run a comment belongs to, compare its updated timestamp against the run.

Read the verdict and reasoning:

```bash
gh pr view <PR> --json comments \
  --jq '[.comments[] | select(.body | test("REVIEW: (PASS|FAIL)"))] | last | .body'
```

#### If the `review` check is green

Proceed to Step 7. Auto-merge has already been armed by the downstream job in the same workflow run.

#### If the `review` check is red

Read the full summary comment, plus any inline diff comments:

```bash
gh api repos/{owner}/{repo}/pulls/<PR>/comments --jq '[.[] | {path: .path, line: .original_line, body: .body}]'
```

Address **every blocking issue**. For observations Claude explicitly flags as non-blocking ("minor", "worth noting", "not blocking on its own"), use judgment: fix them when they are quick and clearly correct; leave them when they need a non-trivial design decision, and say so in the commit message.

The review is prompted to vote FAIL when torn on a `firmware/` change, precisely because CI cannot verify those. A FAIL on firmware is therefore sometimes a request for justification rather than a defect report. If you believe the review is wrong, say why in a PR comment rather than silently pushing an unrelated change: the next review sees the conversation.

After making fixes: commit, push, and let CI (Step 4) and the review (Step 6) re-run on `synchronize`. Repeat until `review` is green. If the same issue survives two fix attempts and you are not making progress, stop and surface it to the user rather than looping.

### Step 7 — Wait for the merge

Nothing in this repo merges the PR. GitHub does, once every required check on the head SHA is green, the PR is not a draft, and the branch is up to date with `main`. Confirm auto-merge is armed, then wait:

```bash
gh pr view <PR> --json autoMergeRequest --jq '.autoMergeRequest'
```

```bash
for i in $(seq 1 20); do
  state=$(gh pr view <PR> --json state,mergedAt --jq '{state: .state, mergedAt: .mergedAt}')
  echo "$state"
  echo "$state" | grep -q '"MERGED"' && break
  sleep 15
done
```

Confirm:

```bash
gh pr view <PR> --json state,mergedAt,mergeCommit \
  --jq '"\(.state) at \(.mergedAt) — merge commit \(.mergeCommit.oid)"'
```

If it is still open after a few minutes with everything green, diagnose in this order:

1. **Is `review` actually a required check?** `gh api repos/{owner}/{repo}/rulesets --jq '.[].id'` then read the ruleset. If the required checks are only `scripts` and `firmware`, the manual setup step in `docs/pr-automation.md` was never done. Say so; do not merge manually to paper over it.
2. **Is auto-merge armed?** If `autoMergeRequest` is null, the `arm auto-merge` job did not run or failed. Check `gh run list --workflow=claude-review.yml --limit 5`.
3. **Is the branch behind `main`?** The ruleset is strict, so a PR whose base moved sits armed and unmerged until the branch is updated. `gh pr view <PR> --json mergeStateStatus` reports `BEHIND`. Update the branch and let the checks re-run.

Only as a genuine last resort, and after telling the user why, merge by hand with `gh pr merge <PR> --squash`.

### Step 8 — Clean up the branch

```bash
git checkout main
git pull origin main
git branch -d <branch-name>
```

The remote branch is deleted automatically by the repo's "Automatically delete head branches" setting. Do not run `git push origin --delete`; the branch is already gone and the command will error.

### Step 9 — Report completion

Tell the user:
- What was built, in a sentence, and how it differs from the prompt if it does.
- PR number, title, and merge commit SHA.
- **What was and was not verified.** If the change touched `firmware/` and was not flashed to a board, say so explicitly. A merged PR with green checks is not a tested firmware change, and this is the moment where that distinction is easiest to lose.
- Any notable issues and how they were resolved (CI failures, review feedback, problems in the delegated run).
- Confirmation that the branch is deleted and `main` is checked out.

## Hard rules

Repo conventions live in `CLAUDE.md` and are not repeated here. These are the rules specific to running this lifecycle:

- Never open a PR from `main`.
- Never force-push or skip hooks (`--force`, `--no-verify`) without explicit user instruction.
- Never merge by hand to get past a stuck gate without telling the user why. A stuck gate usually means a real misconfiguration; see Step 7.
- Never report a change as verified when only CI ran. See Step 9.
