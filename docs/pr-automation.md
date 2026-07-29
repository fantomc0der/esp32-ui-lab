# PR automation

Solo-dev workflow where Claude reviews every pull request and GitHub squash-merges it once the review and CI both agree. Optimized for the case where you are the only committer and don't want to click "Merge" on work that has already been verified.

The design principle here is that **the gate is the ruleset on `main`, not a workflow**. This repo already protects `main` with required status checks, so Claude's verdict is enforced the same way `scripts` and `firmware` are: it is just another required check. Nothing in `.github/workflows/` decides whether a PR may merge. That decision belongs to GitHub, which already does it correctly, and the workflows here only produce the signals it gates on.

## The happy path

1. Create a branch, commit, push. Open a pull request against `main`. Open it as a **draft** if it isn't finished: `gh pr create --draft`.
2. `ci.yml` runs on every push, draft or not, and reports `scripts` and `firmware`.
3. While the PR is a draft, `review` deliberately fails, which also skips the job that arms auto-merge, so a draft can never arm a merge. A red check on a draft is expected; see below for why it is failed rather than skipped.
4. When you're ready, click **Ready for review** (or `gh pr ready`). That fires `ready_for_review`, the review job runs for real, and Claude posts inline comments plus one sticky summary comment ending in `REVIEW: PASS` or `REVIEW: FAIL`.
5. A follow-up step reads that verdict and exits non-zero on `FAIL`, so the `review` check goes red. Because the sticky comment is edited in place rather than reposted, the check status is the reliable signal, not whether the comment looks new.
6. If the review passed, the second job arms GitHub's native auto-merge. It performs no checks of its own: it sets a flag and exits.
7. GitHub merges when *its* conditions hold: every required check green on the head SHA, not a draft, and the branch up to date with `main`. Then it squashes and deletes the branch.

If anything fails, nothing happens and the PR stays open. Push more commits, the review re-runs on `synchronize`, and GitHub re-evaluates. If you decide mid-review that a PR needs more work, convert it back to a draft.

## Files

- `.github/workflows/ci.yml` — the existing hardware-free checks: every board compiles, scripts parse, scripts only call bindings the C layer registers, generated apps are not stale, doc links resolve. Reports `scripts` and `firmware`.
- `.github/workflows/claude-review.yml` — two jobs. `review` runs the Anthropic action and turns the verdict into the job's conclusion. `arm-auto-merge` needs `review` to have succeeded, then calls `gh pr merge --auto --squash`.
- `.github/workflows/claude-fix.yml` — on-demand. Comment `@claude <what to change>` on a PR and Claude pushes the change to the branch.

## Repository state this depends on

Half of this automation is not in the repo. It lives in GitHub settings, where it is invisible to any diff, survives no fork or clone, and can be changed without leaving a trace in the history. This section records what must be true. When the automation misbehaves in a way the workflow logs do not explain, check these first.

**Ruleset on `main`**, active, targeting the default branch, with no bypass actors:

| Rule | Required setting | What breaks without it |
|---|---|---|
| Require a pull request | enabled, 0 approving reviews | Commits reach `main` without ever entering the gate. Approvals stay at 0 deliberately: the review is enforced as a status check, not as a GitHub review, which also sidesteps the rule that a PAT cannot approve its own account's PR |
| Require status checks | `scripts`, `firmware`, `review` | Dropping `review` reduces the gate to CI alone: a `REVIEW: FAIL` still turns the check red, but the PR merges anyway |
| Require branches up to date | enabled | Nothing breaks; this is what makes a PR stall when `main` moves under it, rather than merging stale |
| Block force pushes, block deletion | enabled | History on `main` becomes rewritable |

The status check contexts are **job** names, not workflow names. `review` comes from `jobs.review.name` in `claude-review.yml`, which is why it is lowercase and sits naturally beside `scripts` and `firmware` from `ci.yml`. Adding the workflow name instead produces a check that never reports and blocks every PR forever.

**Merge settings**, under the repository's general settings:

| Setting | Required | What breaks without it |
|---|---|---|
| Allow auto-merge | on | `gh pr merge --auto` fails, so nothing is ever armed and no PR merges by itself |
| Allow squash merging | on | The arming call specifies `--squash` and fails |
| Automatically delete head branches | on | Merged branches accumulate; the workflow does not delete them itself |

**Secrets**, stored as repository Actions secrets:

| Secret | What it is | Used by |
|---|---|---|
| `ANTHROPIC_API_KEY` | API key from `console.anthropic.com/settings/keys` | `claude-review.yml`, `claude-fix.yml` |
| `PR_AUTOMATION_PAT` | Fine-grained PAT scoped to this repo with `Contents: write` and `Pull requests: write` | `claude-review.yml`, `claude-fix.yml` |

The PAT expires. When it does, arming fails and reviews stop being able to push fixes, both with authentication errors rather than anything that reads as a gate problem. Rotate it rather than extending the expiry indefinitely.

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

## Why a draft PR shows a red `review`

Deliberate, and the one place this setup trades looks for correctness. A draft has not been reviewed, so `review` is failed rather than skipped while the PR is a draft, and it stays red until a real review turns it green.

Guarding the whole job with `if: draft == false` reads better and is wrong. A skipped job still publishes a check run, and GitHub counts a `skipped` conclusion as satisfying a required status check, so a PR carrying only a draft-phase skip reports `CLEAN` and `MERGEABLE` the instant it leaves draft, before any review has run on that commit. Arming stays safe either way, since it is gated on a review that actually succeeded, but the required check is decorative in that window, and the window is wider than it first looks: a PR that was armed, converted back to draft, pushed, and marked ready again picks up a fresh skipped `review` on the new commit that reads as green.

So drafts fail the check. The red X means nothing has reviewed this, which is true.

## Two sharp edges

**Auto-merge does not update the branch.** The ruleset sets `strict_required_status_checks_policy`, meaning a PR must be up to date with `main` before merging. GitHub's auto-merge honours that but will not update the branch for you, so if `main` moves while a PR waits, the PR sits armed and unmerged until you update it. With serial one-at-a-time PRs this never comes up. If it starts happening, the fix is to turn strictness off in the ruleset rather than to add a workflow that pushes merges into branches.

**Arming happens after the review, not alongside it.** This is why `arm-auto-merge` lives inside `claude-review.yml` behind `needs: review` rather than in its own workflow triggered by `ready_for_review`. Marking a PR ready does not change the head SHA, and required checks are evaluated per SHA, so a `review` check left over from the draft phase would still be the current one at that instant. A separate arming workflow firing on the same event would race the review it is supposed to be waiting for, and could arm against a stale result. Making arming a downstream job of the review removes the race by construction: there is no moment at which the merge is armed and the review has not run on that exact commit.

## Changing the review workflow itself

`claude-review.yml` cannot review a change to itself. The action refuses to run whenever the file on the branch differs from the copy on the default branch:

> Workflow validation failed. The workflow file must exist and have identical content to the version on the repository's default branch.

That is a security control, and the right one. Without it a pull request could rewrite the review workflow to print `ANTHROPIC_API_KEY` and `PR_AUTOMATION_PAT` into a log, and the review would obligingly run the attacker's edited copy of itself. The workflow reviewing a diff has to be the one `main` already trusts.

The practical effect: a PR touching this file gets no verdict comment, the verdict step fails closed, `review` goes red, and the required check blocks the merge. Drop `review` from the ruleset's required checks, merge, then add it back. Keep such PRs small and separate from functional changes, because the diff genuinely goes unreviewed.

Failing closed is worth preserving here. Treating a missing verdict as a pass would let a PR that edits the review workflow merge unreviewed, which is precisely the attack the validation exists to prevent.

## Overriding the automation

Because `review` is a required check, this gate genuinely blocks: a PR cannot merge without a `REVIEW: PASS`, including by hand. When the Anthropic API is down, or Claude is wrong and cannot be talked out of it, the escape hatches in increasing order of violence are: comment `@claude` explaining why it is wrong and let it re-review; drop the status check requirement from the ruleset temporarily; or merge from a context with bypass permission. There are no bypass actors, so there is no standing bypass, which is deliberate.

## What this does and does not protect against

The load-bearing risk in this repo is specific and worth stating plainly: **CI cannot run the real tests.** `app/selftest.js` and `app/ui-selftest.js` execute on the panel and report over serial, so a hosted runner never runs them. A green `firmware` check means the code compiled, not that it works. Claude reviews what is visible in the diff. Neither catches a byte-order pairing that is now double-correcting, a `JS_DupValue` released twice on teardown, or a touch transform that is subtly wrong, because all three compile fine and all three need the board.

This is why the review prompt tells Claude to weight `firmware/` changes far more heavily than `tools/` changes, and to vote FAIL when genuinely torn on firmware: a wrong FAIL costs one more push, a wrong PASS merges code that nothing on the runner can catch. It is also why "the checks are green" should not be read as "this works" for any change to the binding layer or hardware glue. Those still need a flash and a look at the panel, and the automation does not change that. See [build-and-deploy.md](build-and-deploy.md) for the edit loop.

Secondary risks, in rough order of how likely they are to bite:

- **A false PASS.** LLM review misses subtle logic bugs and anything requiring runtime verification. If it starts rubber-stamping, you will not notice until something breaks on the panel. Read the comments even when they say PASS, and calibrate over time.
- **One reviewer, one blind spot.** A human reviewer gives you a second mental model. Here you get one model's read plus whatever the hardware-free checks happen to cover.
- **Cost scales with pushes.** Every push to a ready PR triggers a full review. Ten fixups mean ten reviews. Each is well under a dollar, but pushing fixups to a draft and marking it ready once is both cheaper and quieter.
- **PAT blast radius.** If the PAT leaks, an attacker can push to branches and merge. Mitigated by the fine-grained scope and an expiry date. Rotate rather than extend.
- **Dependency on API availability.** If the Anthropic API is down, `review` cannot go green and nothing merges automatically until it recovers.
