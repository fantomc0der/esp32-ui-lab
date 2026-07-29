# PR automation

Solo-dev workflow where Claude reviews every pull request and GitHub squash-merges it once the review and CI both agree. Optimized for the case where you are the only committer and don't want to click "Merge" on work that has already been verified.

The design principle here is that **the gate is the ruleset on `main`, not a workflow**. This repo already protects `main` with required status checks, so Claude's verdict is enforced the same way `scripts` and `firmware` are: it is just another required check. Nothing in `.github/workflows/` decides whether a PR may merge. That decision belongs to GitHub, which already does it correctly, and the workflows here only produce the signals it gates on.

## Manual setup step

**This automation does not actually gate on Claude until you add one required status check.** Until then a `REVIEW: FAIL` turns the check red and the PR still merges on CI alone.

Go to **Settings → Rules → Rulesets → `main` → Require status checks to pass**, and add:

```
review
```

so that the required checks read `scripts`, `firmware`, `review`. The context name is the *job* name (`jobs.review.name` in `claude-review.yml`), not the workflow name, which is why it is lowercase and matches the style of the two existing checks.

Everything else is already in place: the ruleset exists and is active, `allow_auto_merge` and `delete_branch_on_merge` are on, squash merging is allowed, and both secrets are set.

## A PR that changes the review workflow cannot be reviewed

The action refuses to run when `claude-review.yml` on the PR branch differs from the copy on the default branch:

> Workflow validation failed. The workflow file must exist and have identical content to the version on the repository's default branch.

This is a security control rather than a bug, and it is the right one. Without it, any PR could rewrite the review workflow to dump `ANTHROPIC_API_KEY` and `PR_AUTOMATION_PAT` into a log, and the review would happily run the attacker's version of itself. The check exists precisely so that the workflow reviewing a diff is the workflow `main` already trusts.

Two consequences, one temporary and one permanent.

**Bootstrapping.** The PR that introduces this automation cannot be reviewed by it, because the workflow does not yet exist on `main`. The action skips, no verdict comment is posted, and the verdict step fails closed, so `review` goes red and the PR is blocked by the very check it is adding. Break the cycle by removing `review` from the ruleset's required checks, merging, and adding it straight back. Once the workflow is on `main`, every later PR reviews normally.

**Editing the review workflow later.** Any PR that touches `claude-review.yml` hits the same wall, permanently. Expect it, and use the same temporary removal. Prefer to keep such PRs small and separate from functional changes, since the diff genuinely goes unreviewed.

Note that the verdict step failing closed is what makes this visible rather than dangerous. A skipped action posts no verdict, "no recognized verdict" is treated as a failure, and the check goes red. The alternative, treating a missing verdict as a pass, would mean a PR editing the review workflow merges unreviewed, which is exactly the attack the validation exists to stop.

## The happy path

1. Create a branch, commit, push. Open a pull request against `main`. Open it as a **draft** if it isn't finished: `gh pr create --draft`.
2. `ci.yml` runs on every push, draft or not, and reports `scripts` and `firmware`.
3. While the PR is a draft, the review job skips. Skipping it also skips the job that arms auto-merge, so a draft can never arm a merge. That is the whole reason drafts are handled by one guard in one place.
4. When you're ready, click **Ready for review** (or `gh pr ready`). That fires `ready_for_review`, the review job runs for real, and Claude posts inline comments plus one sticky summary comment ending in `REVIEW: PASS` or `REVIEW: FAIL`.
5. A follow-up step reads that verdict and exits non-zero on `FAIL`, so the `review` check goes red. Because the sticky comment is edited in place rather than reposted, the check status is the reliable signal, not whether the comment looks new.
6. If the review passed, the second job arms GitHub's native auto-merge. It performs no checks of its own: it sets a flag and exits.
7. GitHub merges when *its* conditions hold: every required check green on the head SHA, not a draft, and the branch up to date with `main`. Then it squashes and deletes the branch.

If anything fails, nothing happens and the PR stays open. Push more commits, the review re-runs on `synchronize`, and GitHub re-evaluates. If you decide mid-review that a PR needs more work, convert it back to a draft.

## Files

- `.github/workflows/ci.yml` — the existing hardware-free checks: every board compiles, scripts parse, scripts only call bindings the C layer registers, generated apps are not stale, doc links resolve. Reports `scripts` and `firmware`.
- `.github/workflows/claude-review.yml` — two jobs. `review` runs the Anthropic action and turns the verdict into the job's conclusion. `arm-auto-merge` needs `review` to have succeeded, then calls `gh pr merge --auto --squash`.
- `.github/workflows/claude-fix.yml` — on-demand. Comment `@claude <what to change>` on a PR and Claude pushes the change to the branch.

## Required secrets

| Secret | What it is | Used by |
|---|---|---|
| `ANTHROPIC_API_KEY` | API key from `console.anthropic.com/settings/keys` | `claude-review.yml`, `claude-fix.yml` |
| `PR_AUTOMATION_PAT` | Fine-grained PAT scoped to this repo with `Contents: write` and `Pull requests: write` | `claude-review.yml`, `claude-fix.yml` |

Both places that use the PAT need it for the same underlying reason: **a push authored by `GITHUB_TOKEN` does not trigger workflows**, which is deliberate on GitHub's part to prevent loops. Auto-merge is performed later by GitHub as whoever armed it, so arming with `GITHUB_TOKEN` would produce a merge commit on `main` that never runs `ci.yml`. `claude-fix.yml` needs it so its push fires `synchronize` and gets re-reviewed rather than sliding in unreviewed.

Unlike some setups, nothing here needs the PAT in order to *approve* a PR. The ruleset requires zero approving reviews, so the self-approval restriction (GitHub forbids a PAT approving a PR opened by the same account) never comes up.

## Verdict format

Claude is prompted to end its summary comment with exactly one of:

```
REVIEW: PASS
REVIEW: FAIL
```

The workflow reads the *last* comment matching that pattern, so a re-review after a fix supersedes an earlier `FAIL`. A missing or unrecognized verdict is treated as a failure rather than a pass, on the principle that a review that did not produce an opinion has not reviewed anything.

## Why there is no auto-merge workflow

The obvious alternative, and the shape this started from, is a workflow triggered by `workflow_run` completion that re-implements the gate in bash: fetch the PR, check it is open, check it is not a draft, check the triggering SHA is still the head, parse the last verdict comment, walk the status check rollup counting pending and failed checks, and only then merge. That is roughly ninety lines, and every one of those conditions is something GitHub already evaluates.

It exists in repos with no branch protection, where a workflow genuinely is the only available gate. That is not this repo. Handing the decision to GitHub removes the bash, the `workflow_run` plumbing, the concurrency group that keeps two runs for the same SHA from racing, and the head-SHA check that exists only because `workflow_run` can fire for a commit that is no longer current. It also removes a class of bug that the hand-rolled version can have and the native one cannot: disagreeing with the ruleset about what "green" means.

There is no `enforce-draft.yml` either. Its job was to convert every new PR to a draft so reviews never fired on unfinished work. Opening the PR as a draft yourself does the same thing without a workflow, a PAT permission, a GraphQL mutation, and a bot comment on every PR. It also removes a race: because that workflow and the review workflow both fire from `opened` and both see the same frozen event payload, the review had to be barred from triggering on `opened` at all to avoid reviewing a PR that was about to become a draft. Without it, `opened` is safe to review on, so a PR opened ready gets reviewed immediately instead of waiting for a click.

## Three sharp edges

**Auto-merge does not update the branch.** The ruleset sets `strict_required_status_checks_policy`, meaning a PR must be up to date with `main` before merging. GitHub's auto-merge honours that but will not update the branch for you, so if `main` moves while a PR waits, the PR sits armed and unmerged until you update it. With serial one-at-a-time PRs this never comes up. If it starts happening, the fix is to turn strictness off in the ruleset rather than to add a workflow that pushes merges into branches.

**A skipped `review` satisfies the required check.** Measured on PR #19, not assumed: with `review` required and only a draft-phase skip recorded on the head SHA, GitHub reports the PR as `CLEAN` and `MERGEABLE` the moment it leaves draft, before any review has run. Skipping a job still publishes a check run, and a `skipped` conclusion counts as success.

This does not let anything merge unreviewed on the normal path, because arming is gated on a review that actually succeeded, so the flag is never set during that window. It does mean the required check is weaker than it looks: a merge performed by any other route in that window would go through. The window is not only the seconds after clicking Ready. A PR that was armed, converted back to draft, pushed, and marked ready again carries a fresh skipped `review` on the new SHA that reads as green.

If that matters more than clean-looking drafts, the fix is to make the draft case *fail* rather than skip, so `review` is red until a real review turns it green. The cost is a red X on every draft, which is honest (nothing has reviewed it) but noisy.

**Arming happens after the review, not alongside it.** This is why `arm-auto-merge` lives inside `claude-review.yml` behind `needs: review` rather than in its own workflow triggered by `ready_for_review`. Marking a PR ready does not change the head SHA, and required checks are evaluated per SHA, so a `review` check left over from the draft phase would still be the current one at that instant. A separate arming workflow firing on the same event would race the review it is supposed to be waiting for, and could arm against a stale result. Making arming a downstream job of the review removes the race by construction: there is no moment at which the merge is armed and the review has not run on that exact commit.

## Overriding the automation

The automation is additive up to the point where you add `review` to the ruleset, and blocking after it. Once it is a required check, a PR cannot merge without a `REVIEW: PASS`, including by hand. If the Anthropic API is down or Claude is wrong and won't be talked out of it, the escape hatches in increasing order of violence are: comment `@claude` explaining why it is wrong and let it re-review; disable the ruleset's status check requirement temporarily; or merge from a context with bypass permission. `bypass_actors` is currently empty, so there is no standing bypass, which is intentional.

## What this does and does not protect against

The load-bearing risk in this repo is specific and worth stating plainly: **CI cannot run the real tests.** `app/selftest.js` and `app/ui-selftest.js` execute on the panel and report over serial, so a hosted runner never runs them. A green `firmware` check means the code compiled, not that it works. Claude reviews what is visible in the diff. Neither catches a byte-order pairing that is now double-correcting, a `JS_DupValue` released twice on teardown, or a touch transform that is subtly wrong, because all three compile fine and all three need the board.

This is why the review prompt tells Claude to weight `firmware/` changes far more heavily than `tools/` changes, and to vote FAIL when genuinely torn on firmware: a wrong FAIL costs one more push, a wrong PASS merges code that nothing on the runner can catch. It is also why "the checks are green" should not be read as "this works" for any change to the binding layer or hardware glue. Those still need a flash and a look at the panel, and the automation does not change that. See [build-and-deploy.md](build-and-deploy.md) for the edit loop.

Secondary risks, in rough order of how likely they are to bite:

- **A false PASS.** LLM review misses subtle logic bugs and anything requiring runtime verification. If it starts rubber-stamping, you will not notice until something breaks on the panel. Read the comments even when they say PASS, and calibrate over time.
- **One reviewer, one blind spot.** A human reviewer gives you a second mental model. Here you get one model's read plus whatever the hardware-free checks happen to cover.
- **Cost scales with pushes.** Every push to a ready PR triggers a full review. Ten fixups mean ten reviews. Each is well under a dollar, but pushing fixups to a draft and marking it ready once is both cheaper and quieter.
- **PAT blast radius.** If the PAT leaks, an attacker can push to branches and merge. Mitigated by the fine-grained scope and an expiry date. Rotate rather than extend.
- **Dependency on API availability.** If the Anthropic API is down, `review` cannot go green and nothing merges automatically until it recovers.
