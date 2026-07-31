---
name: implement-change
description: Takes a change from a one-line prompt all the way to merged. Creates the branch, implements the change by delegating to an autonomous execution skill, opens a draft PR, waits for CI, marks it ready, handles Claude review feedback, confirms the squash-merge lands, and closes the originating GitHub issue if the prompt named one. Use when the user invokes it with the work as an argument, or says "implement and ship X", "build X and get it merged", "take X all the way", or otherwise asks for a change carried to main without further input.
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
- [ ] Step 7: Every review thread answered and resolved
- [ ] Step 8: Merge confirmed
- [ ] Step 9: Issue closed and commented on, if the prompt named one
- [ ] Step 10: Branch cleaned up
- [ ] Step 11: Reported to the user
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

Open it as a draft (`gh pr create --draft ...`). This is load-bearing rather than cosmetic: while the PR is a draft the `review` check fails on purpose, which skips the job that arms auto-merge, so nothing can review or merge work that is still settling. Marking it ready later is the deliberate act that starts the gate.

A red `review` on a draft is expected and is not something to fix. It means nothing has reviewed the PR yet. Do not report it to the user as a CI failure.

### Step 4 — Wait for CI to pass on the draft

`ci.yml` runs on every push regardless of draft state. Wait for the checks on the current head SHA to finish, then judge only the CI ones:

```bash
gh pr checks <PR> --watch || true
gh pr checks <PR> --json name,state --jq '.[] | select(.name == "scripts" or .name == "firmware")'
```

The `|| true` is required, not sloppiness. `review` is failed by design on a draft, so `--watch` always exits non-zero here and would otherwise abort the step or send you investigating a check that is behaving correctly. Only `scripts` and `firmware` decide whether this step passed. The `firmware` job compiles the ESP32 toolchain and is the slow one; several minutes is normal, more on a cold cache.

If `scripts` or `firmware` **fails**, read its log:

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

Proceed to Step 7. Auto-merge has already been armed by the downstream job in the same workflow run, but a green review is not the last gate: any inline comment it left is an open conversation, and those block the merge on their own.

#### If the `review` check is red

Read the full summary comment, plus any inline diff comments:

```bash
gh api repos/{owner}/{repo}/pulls/<PR>/comments --jq '[.[] | {path: .path, line: .original_line, body: .body}]'
```

Address **every blocking issue**.

Non-blocking observations are a different decision, and the default is not to take them. Each one you accept widens the diff, and a wider diff buys another review round over new code, which produces its own remarks: that is the ratchet that turns a small PR into five rounds. Take a non-blocking remark when it is a defect in what you already changed, or when leaving it would make the change itself wrong or misleading. Leave it when it is polish, a preference, or an improvement to code this PR was not otherwise touching, and say so in a reply. "Out of scope, worth doing separately" is a complete answer, and a follow-up issue is a better home for it than this branch.

The exception worth naming, because it is the one case where widening is right: a remark that shows your fix addressed one instance of a defect whose class is wider. Fixing only the instance ships a bug you now know about. Generalize, and say in the PR description that you did and why.

The review is prompted to vote FAIL when torn on a `firmware/` change, precisely because CI cannot verify those. A FAIL on firmware is therefore sometimes a request for justification rather than a defect report. If you believe the review is wrong, say why in a PR comment rather than silently pushing an unrelated change: the next review sees the conversation.

After making fixes: commit, push, and let CI (Step 4) and the review (Step 6) re-run on `synchronize`. Repeat until `review` is green. If the same issue survives two fix attempts and you are not making progress, stop and surface it to the user rather than looping.

**Batch the fixes into one commit and one push.** Every push cancels the review in flight and buys a fresh one, so three pushes in quick succession pay for three reviews of overlapping code and produce three rounds of remarks. Collect everything a round asked for, verify it together, commit once with each item called out in the message, and push once.

**Know when to stop.** Once `review` is green and what remains is non-blocking, the PR is done. Reply on those threads, resolve them, and let it merge. Pushing another commit for polish restarts the whole gate: new CI, a new review over the code you just touched, and a new crop of remarks about it. Two rounds of substantive fixes is normal; a third round arriving with only smaller items than the second is the signal to stop improving and start shipping. If you believe a remaining remark genuinely matters, that is a reason to tell the user, not a reason to push again.

### Step 7 — Answer every review thread, then resolve it

The ruleset on `main` requires conversation resolution, so a single unresolved review thread holds the merge shut no matter how green everything else is. This is easiest to miss on a PASS: all checks pass, auto-merge is armed, and the PR still reports `BLOCKED` with nothing in the check list to explain it. A review that votes PASS while leaving non-blocking inline remarks produces exactly that state, so treat this step as part of every run rather than as something only failed reviews need.

Resolution status is not in the REST comments endpoint, so list the threads over GraphQL:

```bash
gh api graphql -f query='
{
  repository(owner: "OWNER", name: "REPO") {
    pullRequest(number: PR) {
      reviewThreads(first: 50) {
        nodes { id isResolved isOutdated path line comments(first: 1) { nodes { databaseId author { login } } } }
      }
    }
  }
}' --jq '.data.repository.pullRequest.reviewThreads.nodes[]
         | "\(.id)\t\(.comments.nodes[0].databaseId)\tresolved=\(.isResolved)\toutdated=\(.isOutdated)\t\(.path):\(.line)"'
```

That prints both identifiers each thread needs, and they are different things: the `PRRT_...` thread id resolves it, while the numeric `databaseId` of its first comment is the parent the reply attaches to.

For each unresolved thread, reply first and resolve second. Both halves matter: the reply is what a later reader and the next review see, and resolving without one leaves the merge unblocked but the reasoning nowhere.

**Reply on the thread, not at top level.** A reply keeps the answer next to the remark it answers, which is what makes a re-review able to tell an addressed point from a new one:

```bash
gh api repos/{owner}/{repo}/pulls/<PR>/comments/<parent-comment-id>/replies -f body='...'
```

Say which way the decision went and why, in a sentence or two. The useful shapes:

- **Fixed.** Name the commit, so the change can be read without hunting for it, and say what you did if it differs from what was suggested. "Fixed in `<sha>`" alone is thin when the fix took a different route.
- **Changed, but wider or narrower than suggested.** Say what you took and what you left, and why the boundary is there.
- **Deliberately unchanged.** Give the reason and point at where it is already argued, such as being out of scope per the PR description or a trade-off the description makes explicitly. "Out of scope" with nothing behind it reads as a dismissal.
- **Disagreed.** Say what makes the finding wrong and how you established it. A measurement settles it, and the review is instructed to drop a finding an author's measurement contradicts.
- **Partly addressed.** Say which part is done, and leave the thread open for the rest.

**Then resolve, but only what is genuinely finished:**

```bash
gh api graphql -f query='mutation { resolveReviewThread(input: {threadId: "PRRT_..."}) { thread { isResolved } } }'
```

Resolving is a claim that the thread needs no further attention, and it is the one action here that removes a merge gate, so it deserves more care than the reply does. Resolve when the point is fixed, or when it was a preference you have answered and nothing is pending. Leave it open when you have just changed the code the remark is about and want the next review to look, or when you have replied with a disagreement nobody has answered yet.

An **outdated** thread (`isOutdated: true`, its anchor line no longer in the diff) still blocks the merge. It needs the same two steps, not fewer: reply saying where that code went, then resolve.

When a thread genuinely needs the review workflow to look again, get a real re-review rather than resolving on your own judgment, and know what does and does not cause one. A reply, and `@claude` in a comment, do **not** run the review: `claude-review.yml` fires only on `pull_request` events (`opened`, `reopened`, `ready_for_review`, `synchronize`), while `@claude` triggers `claude-fix.yml`. That path re-reviews only if it pushes a commit, because the push is what fires `synchronize`. So a comment alone re-reviews nothing, and the only reliable way to get a fresh verdict is a new commit. If a thread needs a verdict and no code is changing, say so to the user instead of resolving to clear the gate.

`arm auto-merge` needs no attention here. It runs once per reviewed commit that passed, never on a FAIL, and its first step exits early when auto-merge is already set, so it cannot double-arm. Worth knowing that arming is sticky: a PR armed by an earlier commit stays armed after a later push that the review fails, and what holds the merge then is the red `review` check rather than any disarming. "Armed" is therefore not evidence that the current commit passed.

### Step 8 — Wait for the merge

Nothing in this repo merges the PR. GitHub does, once every required check on the head SHA is green, every review thread is resolved, the PR is not a draft, and the branch is up to date with `main`. Confirm auto-merge is armed, then wait:

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

1. **Is a review thread still unresolved?** The likeliest cause by far, and the one the checks do not show: `mergeStateStatus` reports `BLOCKED` while `mergeable` reports `MERGEABLE`, with every check green. Go back to Step 7 and list the threads. Outdated threads count.
2. **Is auto-merge armed?** If `autoMergeRequest` is null, the `arm auto-merge` job did not run or failed. Check `gh run list --workflow=claude-review.yml --limit 5`.
3. **Is the branch behind `main`?** The ruleset is strict, so a PR whose base moved sits armed and unmerged until the branch is updated. `gh pr view <PR> --json mergeStateStatus` reports `BEHIND`. Update the branch and let the checks re-run.
4. **Is `review` actually a required check?** `gh api repos/{owner}/{repo}/rulesets --jq '.[].id'` then read the ruleset. If the required checks are only `scripts` and `firmware`, the manual setup step in `docs/pr-automation.md` was never done. Say so; do not merge manually to paper over it.

Reading the ruleset is what turns a guess into an answer, since it names both the required checks and the conversation-resolution requirement:

```bash
gh api repos/{owner}/{repo}/rulesets/<id> --jq '.rules[] | {type: .type, params: .parameters}'
```

Only as a genuine last resort, and after telling the user why, merge by hand with `gh pr merge <PR> --squash`.

### Step 9 — Close the issue, if the prompt named one

Only applies when the work came in attached to a GitHub issue: the prompt cited an issue number or URL, or the user handed one over as the argument. If no issue was named, skip this step.

Do it **after** the merge is confirmed in Step 8, never before. An issue closed while the PR is still open is a lie about the state of the repo, and reopening it costs more than waiting did.

Leave a comment first, then close:

```bash
gh issue comment <N> --body '...'
gh issue close <N>
```

The comment is short: two or three sentences on what actually merged, plus the PR link. It is read by someone who has the issue open and not the diff, so say what changed rather than restating the issue. Name the same caveat the report carries: if the change touched `firmware/` and was never flashed to a board, say so here too, since this comment outlives the conversation.

The PR body may already have closed the issue on merge, if it carried a `Closes #<N>` line. That is fine and is not a reason to skip the comment: check the state, comment either way, and only run `gh issue close` if it is still open.

```bash
gh issue view <N> --json state,closedAt --jq '"\(.state) \(.closedAt // "")"'
```

Closing an issue is outward-facing and hard to take back, so close only the issue the prompt named. If the merged change happens to resolve others, say so in the comment and leave them to the user.

### Step 10 — Clean up the branch

```bash
git checkout main
git pull origin main
git branch -d <branch-name>
```

The remote branch is deleted automatically by the repo's "Automatically delete head branches" setting. Do not run `git push origin --delete`; the branch is already gone and the command will error.

### Step 11 — Report completion

Tell the user:
- What was built, in a sentence, and how it differs from the prompt if it does.
- PR number, title, and merge commit SHA.
- The issue number and its state, if the prompt named one.
- **What was and was not verified.** If the change touched `firmware/` and was not flashed to a board, say so explicitly. A merged PR with green checks is not a tested firmware change, and this is the moment where that distinction is easiest to lose.
- Any notable issues and how they were resolved (CI failures, review feedback, problems in the delegated run).
- Confirmation that the branch is deleted and `main` is checked out.

## Hard rules

Repo conventions live in `CLAUDE.md` and are not repeated here. These are the rules specific to running this lifecycle:

- Never open a PR from `main`.
- Never force-push or skip hooks (`--force`, `--no-verify`) without explicit user instruction.
- Never merge by hand to get past a stuck gate without telling the user why. A stuck gate is usually an unresolved review thread and occasionally a real misconfiguration, and both are worth diagnosing rather than bypassing; see Step 8.
- Never resolve a review thread without a reply saying what you did about it, and never resolve one to clear the merge gate when the point is still open. Resolving is the one action in this lifecycle that removes a gate on your own authority; see Step 7.
- Never report a change as verified when only CI ran. See Step 11.
- Never close an issue before the merge is confirmed, and never close one the prompt did not name. See Step 9.
