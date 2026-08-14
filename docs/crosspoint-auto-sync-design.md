# CrossPoint Auto-Sync: Design (not yet implemented)

Status: **design only, per explicit instruction. Nothing in this document has
been implemented.** Extends `.github/workflows/update-from-crosspoint.yml`
(currently manual-only) toward a scheduled, classified, selectively
auto-merging sync — real merges only, never cherry-picks, never rebases,
never squashed sync commits.

## 1. Current workflow capability

- **`update-from-crosspoint.yml`** (`workflow_dispatch` only today): fetches
  `crosspoint-reader/crosspoint-reader`'s `develop`, force-updates a Midad-owned
  `upstream` mirror branch (exact copy, zero Midad commits, safe to force-push
  since it has no history of its own), creates a disposable
  `sync/crosspoint-YYYYMMDD-HHMMSS` branch off `develop`, performs a real
  `git merge --no-ff` (never cherry-pick/rebase). Conflict → abort, report file
  list in the step summary, stop. Clean → push the branch, run the full `ci.yml`
  reusable workflow, optionally open a PR (`dry_run: false`).
- **`thin-fork-guard.yml`**: `pull_request`-triggered, advisory only (not
  required) — checks out and runs the PR's *own* copy of the guard scripts, so
  it cannot be trusted as a gate (a PR could edit the guard in the same commit).
- **`thin-fork-guard-trusted.yml`**: `pull_request_target`-triggered, scripts
  always resolved from the base branch (never the PR's own copy), publishes a
  `thin-fork-guard-trusted` commit status against the PR's actual HEAD SHA on
  every `synchronize`. **This is the required, trustworthy evaluator.**
- **Ruleset** (live-verified, id `20797779`, `enforcement: active`): requires
  `Test Status` and `thin-fork-guard-trusted` to pass, `strict_required_status_checks_policy: true`
  (branch must be up to date with `develop` before merge), PR-only (blocks
  direct pushes), 0 required approving reviews (solo-owner repo),
  `bypass_actors: []`, merge methods `[merge, squash]`.
- **CodeQL**: exists (`codeql.yml`), but by its own header comment is
  **deliberately not wired into `Test Status`** and is explicitly "not a
  required check" pending weeks of stability — a standing, deliberate decision,
  not an oversight.
- **CodeRabbit**: reviews PRs today (confirmed via PR #148) but publishes only
  as a PR **review** with state `COMMENTED` — never `APPROVED`/`CHANGES_REQUESTED`
  — and posts **no check-run, no commit status** (empirically verified against
  PR #148's actual check-runs/statuses via the GitHub API). See §6.
- **No auto-merge exists anywhere today** — every merge, including CrossPoint
  syncs, is manual.

## 2. Required changes (summary — detail in §3-§5, §14)

1. Add a `schedule:` trigger alongside the existing `workflow_dispatch`.
2. Add dedup/update-in-place logic before creating a new sync branch.
3. Add a path-based classifier step (A/B/C) after a clean merge.
4. Add a conditional auto-merge step, gated on classification + all checks.
5. Add a durable, git-committed audit-trail entry per sync attempt.
6. Add a small, separately-testable sensitive-path config + classifier script
   (matching the existing `scripts/thin-fork-guard.sh` pattern).

## 3. Class A/B/C classifier

**Class C (conflict)** is decided first, by the merge step itself — unchanged
from today's behavior: `git merge` fails → abort, report, stop. No PR opened.

**Class A vs. B**, once the merge is clean, is decided by scanning the merged
diff's changed-file surface (via `git diff --name-status -M` against the
merge-base, so renames are visible as renames, not silently dropped) against
the sensitive-path policy in §4, **plus** these additional gates, all required
for Class A:

- Zero hits against the sensitive-path policy (including the hard-coded,
  non-overridable `.github/**` rule — see §4).
- `Test Status` and `thin-fork-guard-trusted` both green (the two checks the
  ruleset already requires).
- Default build, sticky build, host unit tests, cppcheck, clang-format all
  green (these already compose `Test Status` via `ci.yml`, restated here for
  clarity — no new check invented).
- CodeQL green, checked by the auto-merge script polling its check-run
  conclusion directly — **not** by adding it to the ruleset's required checks,
  which would contradict that workflow's own documented "not required yet"
  decision (§1). The script can hold a stricter bar than the ruleset's floor.
- CodeRabbit: **advisory only**, logged in the audit trail, never blocking
  (see §6 for why).
- The sync branch is still based on `develop`'s current tip — re-checked
  explicitly by the script immediately before calling `gh pr merge` (defense
  in depth; the ruleset's own `strict_required_status_checks_policy` is the
  real, server-enforced backstop, see §7).

Anything that doesn't clear every Class-A gate is **Class B**: the PR is
opened, all checks still run (so a human reviewing it sees full signal), but
nothing calls `gh pr merge`.

## 4. Sensitive-path policy

Deliberately a small set of **glob-pattern categories**, not an exhaustive
file inventory (per your "avoid a giant permanent allowlist" instruction) —
periodically reviewed, not maintained file-by-file. Proposed starting set,
in a new `.github/thin-fork-sensitive-paths.yml`:

```yaml
# Any path matching a pattern below routes an otherwise-clean sync merge to
# Class B (human review) instead of Class A (auto-merge). Categories, not an
# exhaustive file list -- extend by pattern, not by enumerating files.

freeink_hardware:
  - "freeink-sdk"              # the gitlink itself -- see the special-case note below
  - "lib/hal/**"
  - "platformio.ini"
  - "partitions.csv"
  - "sdkconfig*"
  - "lib/GfxRenderer/**"
  - "src/platform/**"
  - "**/*XteinkDetect*"
  - "lib/hal/HalGPIO*"
  - "src/MappedInputManager*"
  - "src/util/ButtonNavigator*"
  - "src/util/GridNav*"
  - "**/*PowerManager*"
  - "**/*Sleep*"
  - "**/*Wake*"

security:
  - "src/network/OtaUpdater*"
  - "src/network/FirmwareFlasher*"
  - "src/network/OtaBootSwitch*"
  - "src/network/HttpDownloader*"
  - "lib/KOReaderSync/**"
  - "scripts/sign_firmware.sh"
  - "docs/ota-signing-key-management.md"
  - "ota-signing-public-key.bin"
  - ".github/**"                # see the hard-coded rule below -- listed for documentation, enforced unconditionally

midad_product_deltas:
  - "lib/ArabicShaper/**"
  - "lib/MiniBidi/**"
  - "lib/I18n/translations/arabic.yaml"
  - "lib/I18n/translations/farsi.yaml"
  - "src/activities/util/KeyboardEntryActivity*"
  - "lib/Epub/Epub/ParsedText*"        # Tier-1 Arabic/RTL/kashida logic, per the convergence audit
  - "src/components/themes/foulad/**"
  - "**/*Foulad*"
  - "**/*Midad*"
  - "lib/hal/BlePeripheralManager*"
  - "src/BleCommandDispatcher*"
  - "src/CrossPointSettings.h"          # UI_THEME enum, language/theme architecture

reader_architecture:
  - "src/activities/reader/**"
  - "src/activities/UiListActivity*"
  - "src/activities/UiTabListActivity*"
```

**Two rules are hard-coded in the classifier script, not overridable by
editing the YAML, and checked unconditionally regardless of what the config
says**:
1. **Any diff touching `.github/**`** (including the auto-sync workflow
   itself, `thin-fork-guard*.yml`, and any of CrossPoint's own `.github/`
   files that a full-tree merge would otherwise try to merge in) is
   automatically Class B. This directly covers "upstream modifying GitHub
   workflows," "changes to the auto-sync workflow itself," and "changes to
   thin-fork/security evaluator files."
2. **Any change to the `freeink-sdk` gitlink itself** (i.e. CrossPoint's own
   commit history moving *their* freeink-sdk submodule pointer, which would
   appear in the merge diff as a gitlink change at that path) is automatically
   Class B — a submodule-pointer change can't be evaluated by content-diff
   path globbing the same way a normal file change can, and covers "upstream
   changing submodule pointers" explicitly.

Renames are covered because the classifier reads `git diff --name-status -M`
output, which reports a rename as `R<score> old_path new_path` — both paths
are checked against the policy, so a rename that moves a file **into** or
**out of** a sensitive category is caught either direction.

## 5. Scheduling / deduplication design

- `schedule: cron: '0 6 * * *'` (daily), `workflow_dispatch` kept for manual/dry-run.
- Before creating a new sync branch: check for an existing open PR carrying a
  dedicated label (e.g. `crosspoint-sync`) via `gh pr list --search "is:open label:crosspoint-sync"`.
  - **No open PR, upstream unchanged since the last recorded sync** (compare
    against the audit trail's last-recorded upstream SHA, §9): do nothing, per
    "if nothing changed upstream: do nothing."
  - **No open PR, upstream has new commits**: proceed as today (new branch, new PR).
  - **An open PR exists, and upstream has moved further since that PR's
    captured SHA**: **update it in place** — force-push a new merge commit
    onto the *same* disposable sync branch (re-merging the newer `upstream`
    ref), which re-triggers CI and re-classifies. This force-push targets a
    Midad-owned disposable branch, never `develop` — a fundamentally different,
    safe operation from the force-push-to-`develop` prohibition elsewhere in
    this project's policy. One continuous PR thread, not a pile of duplicates.
  - **An open PR exists, upstream unchanged**: skip, exactly per "never create
    duplicate sync PRs for the same upstream SHA."
- **Race protection**: a `concurrency: group: crosspoint-auto-sync, cancel-in-progress: false`
  block on the workflow ensures two triggers (e.g. a slow scheduled run
  overlapping a manual dispatch) queue rather than run concurrently and
  duplicate work — and never cancels a run that's mid-merge.

## 6. CodeRabbit feasibility — investigated, not assumed

**Empirically checked** against PR #148 (`gh api .../commits/<sha>/check-runs`
and `.../pulls/148/reviews`): CodeRabbit posts a PR **review** with state
`COMMENTED` — GitHub's branch-protection model only recognizes `APPROVED`/
`CHANGES_REQUESTED` review states for gating purposes, and `COMMENTED` carries
no pass/fail semantics at all. There is **no check-run and no commit status**
from CodeRabbit on this repo as currently configured — confirmed by listing
every check-run and status on that commit and finding zero CodeRabbit entries.

**Conclusion: CodeRabbit cannot be a deterministic auto-merge gate today.**
Per your instruction, it stays **advisory only** — its review comment is
recorded in the audit trail (link + timestamp) for a human to read on any
Class B PR, but Class-A auto-merge does not wait for or depend on it, and the
classifier never parses its comment text. If CodeRabbit's own account
settings were ever changed to publish a structured check-run (some CodeRabbit
plans support this), this could be revisited — not true of the current setup.

## 7. Ruleset interaction

**No ruleset change is required for Class A merges to work.** The ruleset
already requires exactly the two checks (`Test Status`, `thin-fork-guard-trusted`)
a clean, non-sensitive upstream merge would naturally produce and pass — a
script calling `gh pr merge --merge` when those are green is not bypassing
anything; **GitHub itself will refuse the merge server-side** if either check
is red, if the branch is stale relative to `develop`
(`strict_required_status_checks_policy: true`), or if anything else about the
PR doesn't satisfy the ruleset. This is a strong, structural safety property:
even a buggy auto-merge script cannot force a merge the ruleset would reject —
the worst a bug can do is call `gh pr merge` and have GitHub say no.
`bypass_actors: []` remains empty; no admin bypass; no temporary disabling
needed for ordinary Class-A runs — a genuine clean merge naturally satisfies
`thin-fork-guard-trusted`, exactly as you specified. The existing emergency-
maintenance procedure (temporarily disable the ruleset for a guard-fix PR)
stays unchanged and unused by ordinary auto-sync operation.

**Note on scope**: `thin-fork-guard-trusted` answers "is this PR safe from a
CI-security self-judging perspective" (can the PR modify the evaluator
grading it). The new path-based classifier in §3/§4 answers a *different*
question — "does this PR touch an area where CrossPoint's own testing doesn't
give Midad enough confidence to skip human review." They're complementary
gates, not duplicates.

## 8. Race-condition protection

- Dedup (§5) prevents duplicate PRs for the same upstream state.
- `concurrency:` (§5) prevents two workflow runs from executing simultaneously.
- Between "checks report green" and "auto-merge executes," `develop` could in
  principle move (e.g. a Midad-authored PR merges first). The ruleset's
  `strict_required_status_checks_policy: true` is the enforced backstop —
  GitHub rejects a merge attempt against a now-stale branch. The auto-merge
  script also re-verifies `develop`'s current tip immediately before calling
  `gh pr merge` (defense in depth, and produces a clearer audit-log message
  than a bare API rejection).
- No blind retry loop anywhere: a rejected auto-merge attempt is logged and
  left for the next scheduled run's dedup logic to reconsider, not retried
  in-place.

## 9. Failure / rollback behavior

- **Class C**: unchanged from today — merge aborted, conflict report posted,
  no branch/PR side effects, nothing pushed.
- **Class A whose checks fail**: the PR stays open, unmerged — it effectively
  becomes Class B in practice (needs a human), never auto-retried blindly.
- **Auto-merge API call itself fails** (stale branch, ruleset rejection,
  transient API error): logged clearly in the run summary and the audit
  trail, PR stays open, no retry loop — the next scheduled run's dedup logic
  naturally reconsiders it.
- **No automated rollback of a completed merge is proposed.** Prevention
  happens before merge (the ruleset + classifier); once a Class-A merge
  commit lands, undoing it is an ordinary manual `git revert`, identical to
  reverting any other merged PR — no special machinery needed or desired.

## 10. Exact Git ancestry produced

Unchanged in shape from the existing manual flow and from PR #148's own
precedent: `upstream` mirror = exact CrossPoint `develop` copy (force-updated,
zero Midad commits, no history of its own to lose). Disposable
`sync/crosspoint-YYYYMMDD-HHMMSS` branch = a real `git merge --no-ff` of
`upstream` into a branch cut from Midad's `develop`. On Class-A auto-merge:
`gh pr merge --merge` (GitHub's "Create a merge commit" method — the only
method used for sync PRs, never squash/rebase, matching the existing PR-body
instruction and this project's standing policy). Resulting `develop` history:
ordinary linear Midad commits, punctuated by two-parent merge commits whose
second parent is CrossPoint's real `develop` history at that SHA — real
ancestry, never manually recreated, exactly as required.

## 11. Example: a safe automatic update

CrossPoint lands a small, self-contained perf fix in
`lib/Epub/Epub/CssParser.cpp` (no `freeink-sdk`, no `.github/`, no reader-activity,
no Midad-product-delta path touched). Merge is clean (no textual conflict),
classifier finds zero sensitive-path hits, `Test Status` +
`thin-fork-guard-trusted` + CodeQL all green. → **Class A**: PR opens, CI runs,
`gh pr merge --merge` fires automatically once checks post green, branch
deleted after merge, audit-trail entry recorded.

## 12. Example: clean merge, human review required

CrossPoint adds a new field to `BoardConfig.h` (a `freeink_hardware`-category
path). The merge is completely conflict-free — git reports zero conflicts —
but the classifier flags the hardware-config touch. → **Class B**: PR opens,
every check still runs (and may well all pass), but no auto-merge call is
made. It sits for a human, exactly as intended, even though nothing about the
merge itself was messy.

## 13. Example: a conflicted update

CrossPoint modifies `lib/MiniBidi/minibidi.c` again (as it did previously with
`do_shape()`), and Midad's own file has diverged enough that `git merge`
reports a real conflict. → **Class C**: merge aborted (`git merge --abort`),
no branch pushed, no PR opened. Report posted (matching today's existing
report format) with: upstream SHA, prior Midad `develop` SHA, the conflicted
file list, and a subsystem grouping (here: "Arabic/bidi text engine — see
`docs/crosspoint-freeink-convergence-audit.md` item 10 for why this file is
already known to diverge").

## 14. Implementation files that would change (when implemented — not now)

- `.github/workflows/update-from-crosspoint.yml` — add `schedule:`, dedup
  logic, a call into the new classifier script, the conditional auto-merge
  step, and the audit-trail-append step.
- **New** `.github/thin-fork-sensitive-paths.yml` — the classifier's pattern
  config (§4), kept out of workflow YAML for maintainability.
- **New** `scripts/classify-sync-paths.sh` — takes a changed-file list (from
  `git diff --name-status -M`) plus the config above, outputs Class A/B/C and
  the specific reason(s), matching the existing `scripts/thin-fork-guard.sh`/
  `scripts/measure-conflicts.sh` pattern (a real, independently-testable
  script, not inline YAML logic).
- **New** `scripts/test-classify-sync-paths.sh` — unit tests for the
  classifier (glob matching, rename handling, the two hard-coded rules),
  matching the existing `scripts/test-thin-fork-guard.sh` pattern.
- **New**, small: an audit-trail append step/script — proposed as a commit to
  a durable, git-tracked `docs/sync-audit-log.jsonl` (one JSON line per sync
  attempt: previous CrossPoint ancestor, new CrossPoint SHA, Midad base SHA,
  included CrossPoint commit list, changed files, class, reason, check
  results, resulting merge SHA) made as a small follow-up commit after a
  successful merge (or after opening a Class-B/C PR) — durable and diffable,
  unlike ephemeral Action run logs which expire.
- `docs/upstream-sync-architecture.md` — updated to document the new design,
  once actually implemented (explicitly not now).

**Note on the classifier's own trust model**: unlike a contributor-opened PR,
the sync branch here is constructed entirely by Midad's own first-party
scheduled workflow from freshly-fetched upstream content — the "a PR can edit
the evaluator judging it" problem `thin-fork-guard-trusted.yml` was built to
close doesn't apply the same way here, since there's no adversarial PR author
in this flow. The one real analog: CrossPoint's own `.github/` files
participating in the merge, which is exactly why rule 1 in §4 is hard-coded
and unconditional rather than left to the editable YAML config.

## 15. Test plan

1. **Dry-run extension**: `workflow_dispatch` with `dry_run: true` should
   exercise fetch → merge → classify (prints the resulting class and reasons)
   without opening a PR or attempting any merge — extends today's existing
   dry-run behavior, doesn't replace it.
2. **Staged rollout, matching the precedent already set for
   `thin-fork-guard-trusted.yml`'s own validation**: land the scheduled
   trigger and classifier with **auto-merge disabled** first (every sync
   still opens a PR, classified but never auto-merged) for a real validation
   period. Only enable the actual `gh pr merge` call once classification
   accuracy has been confirmed across several real scheduled cycles.
3. **Disposable test-PR validation** (same repo, no fork needed — this repo
   has none today, matching the constraint already documented in
   `thin-fork-guard-trusted.yml`'s own header): construct, one at a time —
   (a) a clean, no-sensitive-path test merge → confirm Class A is computed
   correctly; (b) a test merge touching one path from each §4 category →
   confirm Class B in each case; (c) a deliberately conflicting test merge →
   confirm Class C, correct report; (d) a synthetic already-open-PR scenario →
   confirm dedup logic updates in place rather than duplicating; (e) a test
   merge touching `.github/**` specifically → confirm the hard-coded rule
   fires regardless of what the YAML config says; (f) confirm an audit-log
   entry is appended correctly for each of A/B/C.
4. Only after all of the above pass on live GitHub infrastructure (not just
   locally) should the scheduled trigger run unattended with auto-merge armed.

---

## Can this be done safely with the current GitHub/ruleset architecture?

**YES**, with two caveats already built into the design above, not treated as
blockers: (1) CodeRabbit is advisory-only, never a hard gate — the current
integration provides no deterministic machine-readable signal (§6); (2) CodeQL
is checked by the auto-merge script directly, not added to the ruleset's
required checks, respecting that workflow's own explicit "not required yet"
decision (§1, §3). Everything else — scheduling, dedup, path classification,
conditional auto-merge — is a straightforward extension of infrastructure that
already exists and is already proven (the ruleset is live and active, the
trusted evaluator is already the required gate, the real-merge mechanism is
already validated via PR #148). The ruleset's server-side enforcement means
the auto-merge script's own correctness is not the only thing standing between
a bad merge and `develop` — GitHub itself refuses anything that doesn't
satisfy the active ruleset, which is what makes this safe to build with
confidence rather than needing a fundamentally different architecture first.
