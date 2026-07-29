---
name: ship-pr
description: After code changes are complete, autonomously open a PR, wait for CI, mark it ready, handle Claude review feedback, and verify the auto-merge lands. Covers the full lifecycle from "code is done" to "merged to main" without user intervention.
compatibility: Requires git and gh CLI, both installed and authenticated.
---

# Ship PR

Autonomously carry finished work through this repo's full PR lifecycle: create a feature branch, open the PR as a draft, wait for CI, mark it ready, address any Claude review feedback, confirm the squash-merge completes, and clean up. No manual steps by the user.

## When to invoke

Invoke this skill when:
- The user explicitly asks for hands-off PR handling ("ship it", "open a PR and handle it", "auto-merge this", "use ship-pr").
- The user's prompt included this skill name as a suffix or qualifier, e.g. `/{omc-command} make my code do X /ship-pr` or `/{omc-command} ... then ship-pr`.
- Work was done via an OMC command (autopilot, ralph, ultrawork, etc.) and the user indicated upfront they want the full PR flow handled automatically.

Do **not** invoke this skill automatically just because code changes were made. Only fire it when the user has signalled they want hands-off PR handling.

## Assumptions and preconditions

- All code changes are **committed**. If `git status --porcelain` shows uncommitted changes, commit them with a sensible message before proceeding (do not silently drop them, and do not silently sweep in noise like `.temp/`).
- If any `app/src/*.jsx` file changed, `node tools/build-app.mjs` has been run and the regenerated outputs are committed. CI fails on a stale generated app, so this is worth checking before opening anything.
- This repo uses the automation documented in `docs/pr-automation.md`: `ci.yml` reports `scripts` and `firmware`, `claude-review.yml` reports `review` and then arms GitHub's native auto-merge, and GitHub itself performs the squash once the ruleset on `main` is satisfied. There is no auto-merge workflow to wait on or debug.

## Workflow steps

### Step 1 — Create and switch to a new feature branch

Ensure the work is on a dedicated feature branch based off `origin/main`. Never open a PR directly from `main`.

```bash
git fetch origin main
git checkout -b <branch-name> origin/main
```

Choose a short, descriptive branch name in `kebab-case` that reflects the work (e.g. `fix-touch-axis-transpose`, `add-slider-binding`).

If you are already on a feature branch created for this work, skip the checkout and just ensure the commits are present on it.

### Step 2 — Open the PR as a draft

Follow the `create-pull-request` skill (`.claude/skills/create-pull-request/SKILL.md`) in full for all push, diff-review, title, and body steps. Capture the PR number and URL.

Open it as a draft (`gh pr create --draft ...`). This is load-bearing rather than cosmetic: while the PR is a draft the review job skips, which also skips the job that arms auto-merge, so nothing can review or merge half-finished work. Marking it ready later is the deliberate act that starts the gate.

### Step 3 — Wait for CI to pass on the draft

`ci.yml` runs on every push regardless of draft state. Poll until the checks on the current head SHA complete:

```bash
gh pr checks <PR> --watch
```

Expect `scripts` and `firmware`. The `firmware` job compiles the ESP32 toolchain and is the slow one; several minutes is normal, more on a cold cache.

If a check **fails**, read its log:

```bash
gh run view <run-id> --log-failed
```

Diagnose it. Common failures here and what they actually mean:
- `build-app.mjs --check` failing means a generated app is stale. Run `node tools/build-app.mjs` and commit the output. If it reports all outputs stale on Windows, suspect line endings before suspecting the build.
- `check-js-api.mjs` failing means a script calls a binding the C layer does not register, or calls a widget method on the wrong kind of widget. This is a real bug that would only otherwise surface on the device.
- The doc link check failing means a relative link in a `.md` file points at a file that does not exist.

Fix the code, commit, push. CI re-runs automatically. Repeat until green.

If the failure is clearly infrastructure noise (runner flake, network timeout) and the same check passed on a prior run, re-run it:

```bash
gh run rerun <run-id> --failed
```

Do not re-run speculatively. Only when there is clear evidence of a transient failure.

### Step 4 — Mark the PR ready for review

Once CI is green:

```bash
gh pr ready <PR>
```

This fires `ready_for_review`, which triggers the review. Do not mark ready before CI is green: it costs a review of code that is about to change, and every push to a ready PR triggers another one.

### Step 5 — Wait for the Claude review

The verdict is the `review` check's conclusion, so watch the check rather than the comment:

```bash
gh pr checks <PR> --watch
```

The check is authoritative and the comment is the explanation. The summary comment is **sticky**, edited in place rather than reposted, so "the comment looks new" is not a signal that a re-review happened. If you need to confirm which run a comment belongs to, compare its updated timestamp against the run.

Read the verdict and reasoning:

```bash
gh pr view <PR> --json comments \
  --jq '[.comments[] | select(.body | test("REVIEW: (PASS|FAIL)"))] | last | .body'
```

#### If the `review` check is green

Proceed to Step 6. Auto-merge has already been armed by the downstream job in the same workflow run.

#### If the `review` check is red

Read the full summary comment, plus any inline diff comments:

```bash
gh api repos/{owner}/{repo}/pulls/<PR>/comments --jq '[.[] | {path: .path, line: .original_line, body: .body}]'
```

Address **every blocking issue**. For observations Claude explicitly flags as non-blocking ("minor", "worth noting", "not blocking on its own"), use judgment: fix them when they are quick and clearly correct; leave them when they need a non-trivial design decision, and say so in the commit message.

Note that the review is prompted to vote FAIL when torn on a `firmware/` change, precisely because CI cannot verify those. A FAIL on firmware is therefore sometimes a request for justification rather than a defect report. If you believe the review is wrong, say why in a PR comment rather than silently pushing an unrelated change: the next review sees the conversation.

After making fixes:
1. Commit with a message describing what was addressed.
2. Push.
3. CI re-runs on `synchronize` (Step 3 logic).
4. The review re-runs on `synchronize` (Step 5 logic).

Repeat until `review` is green. If the same issue survives two fix attempts and you are not making progress, stop and surface it to the user rather than looping.

### Step 6 — Wait for the merge

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

### Step 7 — Clean up the branch

```bash
git checkout main
git pull origin main
git branch -d <branch-name>
```

The remote branch is deleted automatically by the repo's "Automatically delete head branches" setting. Do not run `git push origin --delete`; the branch is already gone and the command will error.

### Step 8 — Report completion

Tell the user:
- PR number, title, and merge commit SHA.
- A one-line summary of what changed.
- **What was and was not verified.** If the PR touched `firmware/` and was not flashed to a board, say that explicitly. A merged PR with green checks is not a tested firmware change, and this is the moment where that distinction is easiest to lose.
- Any notable issues and how they were resolved (CI failures, review feedback).
- Confirmation that the branch is deleted and `main` is checked out.

## Hard rules (inherited from CLAUDE.md)

- Never hard-wrap prose in `docs/`, `README.md`, PR bodies, or commit message bodies. One continuous line per paragraph or list item. Code comments are the exception and follow the surrounding code's width.
- Never alter whitespace on lines you are not otherwise changing.
- Never hand-edit a generated file. `app/apps/*.js`, `app/app.js` and `app/ui-selftest.js` come from `app/src/*.jsx` via `node tools/build-app.mjs`.
- Never write tombstone or changelog comments in source ("no longer needed", "previously did X"). Git history is the record.
- Never force-push or skip hooks (`--force`, `--no-verify`) without explicit user instruction.
- Never open a PR from `main`.

## OMC integration note

This skill is designed to be chained after OMC execution commands:

```
/autopilot make my code do X — use ship-pr when done
/ralph implement Y and ship-pr it
```

Treat "ship-pr", "ship pr", "auto-merge", and "hands-off PR" as signals to invoke it after the primary work command completes. It fires *after* the code work is finished, not concurrently. If OMC work runs in multiple passes, wait until the final pass is complete and everything is committed before Step 1.
