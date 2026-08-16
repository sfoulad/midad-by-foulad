#!/usr/bin/env bash
# thin-fork-guard.sh <base-ref> <head-ref> [upstream-ref] [head-branch-name]
#
# Reports, and gates on, three THIN-FORK things <head-ref> may do relative
# to <base-ref>, all measured against <upstream-ref> -- the caller's job to
# resolve to a genuinely current CrossPoint ref before invoking this script
# (both thin-fork-guard.yml and thin-fork-guard-trusted.yml fetch
# crosspoint-reader/crosspoint-reader directly, read-only, for exactly this
# reason -- see their own headers). [upstream-ref] falls back to
# `origin/upstream` only for manual/local invocation convenience; that
# mirror branch is maintained by .github/workflows/update-from-crosspoint.yml
# for ITS OWN sync mechanics (an ancestry witness, a disposable sync-branch
# base) and can silently go stale -- a failed sync dispatch leaves it
# pinned at an old commit with no visible symptom until this guard
# misjudges a PR against it (see docs/upstream-sync-architecture.md's
# incident note). Neither guard workflow relies on it being fresh; do not
# reintroduce that dependency without re-reading that incident. PLUS two
# independent SECURITY-BOUNDARY gates (sections 6-7 below) that protect the
# guard's own enforcement machinery and are not about thin-fork divergence
# at all:
#
#   1. Introduce new fixed-upstream MERGE-CONFLICT surface (simulated real
#      merge of <upstream-ref> into each side).
#   2. Introduce new MIDAD-SIDE DIVERGENCE in a shared/upstream file that
#      was clean (untouched by Midad) relative to the last history Midad's
#      base actually shared with upstream.
#   3. RENAME a shared file away from its upstream path while that file was
#      still Midad-clean -- a special case of #2 that the general check
#      cannot see on its own (see the note above compute_diverged_files).
#   4. MODIFY, DELETE, or RENAME (either direction) a security-boundary
#      file (section 6) -- the trusted workflow, the CI entrypoint, this
#      guard's own scripts, AND the self-test suite (scripts/test-thin-
#      fork-guard.sh): the trusted workflow executes the self-test BEFORE
#      the production guard, using the base branch's copy, so it is
#      trusted executable code, not just a governance-flagged file -- a
#      compromised self-test in a merged base could alter the checkout
#      (overwrite thin-fork-guard.sh with a stub, etc.) before the real
#      evaluator ever runs. Independent of upstream ownership, existing
#      divergence, or conflict count: these files define enforcement
#      itself, so any ordinary-PR touch to them fails, full stop. Checked
#      via both `git diff --name-only` (covers plain modify/delete) AND
#      `git diff --name-status --diff-filter=R` inspecting BOTH the old and
#      new path of every detected rename -- a plain `--name-only` diff does
#      not reliably surface a rename's source path (see the note above
#      is_shared_path), so relying on it alone would let a rename-away (or
#      a rename-into a protected pathname) slip past silently. See
#      "Security-boundary file maintenance" in
#      docs/upstream-sync-architecture.md for the explicit human procedure
#      required to change one legitimately.
#   5. INTRODUCE a second status/check-writing workflow (section 7) --
#      `.github/workflows/*.yml` at HEAD other than
#      thin-fork-guard-trusted.yml itself must never SEMANTICALLY grant
#      `statuses: write`, `checks: write`, or the `write-all` shorthand.
#      One invariant: no ordinary PR may create another workflow capable of
#      spoofing the trusted required status. Each workflow file is parsed
#      as YAML (scripts/check-workflow-permissions.rb) and its actual
#      resolved permission values are inspected -- not a text/regex match
#      against one exact spelling, which quoting, spacing, or a YAML
#      alias/anchor can trivially differ from while meaning the same
#      thing. A file this guard cannot read or parse is treated as a hard
#      error (fail-closed), never as "contains no forbidden permissions."
#
# --- Why #2 is anchored to a merge-base, not to upstream's current tip ---
# A naive two-state comparison ("does <base-ref>/<head-ref>'s copy of this
# file differ from <upstream-ref>'s copy, right now") conflates two very
# different situations:
#   (a) Midad edited the file itself, diverging it from upstream.
#   (b) Midad never touched the file at all -- upstream simply moved ahead
#       since the last time Midad's base actually synced with it.
# A file Midad has never edited can fail a "differs from upstream's CURRENT
# tip" check purely because of (b), which then makes the naive check
# misclassify it as "already diverged" (implying Midad caused it, and
# implying any further edit is not new). That's wrong: if a PR then edits
# that same file for the first time, it IS introducing new Midad-side
# divergence, and the naive check would silently let it slide into
# "already diverged, report only" instead of failing it.
#
# The fix: compute COMMON_REF = merge-base(<base-ref>, <upstream-ref>) --
# the last point in history <base-ref> and upstream genuinely shared. For
# each shared path, compare four blobs: COMMON (the shared ancestor),
# BASE, HEAD, and UPSTREAM (upstream's current tip):
#   - HEAD == UPSTREAM                        -> converged (PASS, always)
#   - BASE == COMMON and HEAD == BASE          -> untouched, not evaluated
#   - BASE == COMMON and HEAD != BASE/UPSTREAM -> NEW Midad-side divergence
#                                                  (FAIL: first edit to a
#                                                  file that was clean
#                                                  relative to shared
#                                                  history)
#   - BASE != COMMON (Midad had already diverged this file before this PR,
#     independent of whatever upstream has done since)
#                                              -> pre-existing divergence,
#                                                 report only, not new
# See classify_divergence() below for the exact implementation. The older,
# simpler "does this differ from upstream's current tip" comparison is
# still computed and reported separately (labeled "vs. current upstream
# tip") because it answers a different, still-useful question -- how far a
# file has drifted from upstream *today* -- but it is no longer used to
# decide FAIL/PASS on its own.
#
# See CLAUDE.md's "Midad Thin-Fork Architecture" section and
# docs/upstream-sync-architecture.md for why any of this matters.
#
# Deliberately compares against whatever <upstream-ref> resolves to at run
# time, not a hard-coded historical commit: base and head are measured
# against the identical snapshot within one run, so the only variable is
# the PR's own diff -- that stays correct forever, including after the
# first full CrossPoint merge, without ever needing today's number
# hard-coded. This only holds if the caller actually resolved <upstream-ref>
# freshly for this run (see the top-of-file note on why the caller, not
# this script, owns that).
#
# A recognized genuine CrossPoint sync PR (see is_recognized_sync_pr below)
# is NOT exempted from any of these -- a real sync is expected to pass all
# three on its own merits (it does not introduce a NEW conflict/divergence
# that didn't already exist on either side; it resolves them). Recognition
# is purely informational in the report. This is intentional: exempting
# sync PRs previously meant a later Midad commit riding on a valid sync's
# ancestry could diverge a previously-clean upstream file for free, which
# is exactly the failure mode this gate exists to catch.
#
# Exit 0: no new conflicting files, no new Midad-side divergence, no
#         rename of a previously Midad-clean shared file away from
#         upstream, no security-boundary file touched, and no other
#         workflow introduces status/check-write permissions.
# Exit 1: otherwise, OR if conflict measurement itself fails for a non
#         content-conflict reason (see measure-conflicts.sh -- this script
#         does not catch that failure, so it propagates as a hard abort
#         under `set -e`, which is the fail-closed behavior we want: a
#         broken measurement must never be silently read as "0 conflicts,
#         PASS"), OR if <base-ref> and <upstream-ref> share no common
#         ancestor at all (merge-base itself fails -- cannot reason about
#         divergence without one).
#
# --- Baseline-aware classification (adopted-upstream-blob) ------------------
# A long-running PR that performs its own real `git merge --no-ff` of
# <upstream-ref> directly on its branch (not yet via <base-ref>) creates a
# blind spot in the four-state table above: the moment ANY of that
# real-merge content lands on HEAD, `classify_divergence` sees
# `base(==common) != head` and reports "new_divergence" -- indistinguishable
# from a hand-authored Midad edit -- even though the content came from a
# genuine, ancestry-verified merge of upstream and Midad wrote none of it.
# Worse, if <upstream-ref> keeps moving afterward (as it always will), a PR
# that has not changed AT ALL since its own real merge starts failing this
# gate purely because the live comparison target moved, not because of
# anything the PR did. See docs/upstream-sync-architecture.md's "Baseline-
# aware divergence classification" section for the incident (PR #149,
# src/activities/reader/TxtReaderActivity.cpp) that surfaced this.
#
# find_adopted_upstream_blob() answers a narrower, per-path question:
# "did this PR itself, via a real, ancestry-verified merge, ever adopt a
# specific upstream blob for this path?" -- and if so, treats HEAD matching
# THAT blob as good news (a new `adopted_baseline` classification state),
# independent of where <upstream-ref> has drifted to since. It reuses the
# exact same trust primitive as is_recognized_sync_pr() below (a merge
# commit's parent must be an ancestor of the freshly-fetched, live
# <upstream-ref> -- not a text/branch-name match, not attacker-forgeable
# without controlling the real crosspoint-reader remote), but drops that
# function's stronger "head has fully caught up to today's tip" precondition,
# since a staged/partial real merge (this PR's actual situation) never
# satisfies that and was never meant to.
#
# A candidate merge M qualifies for a path ONLY if BOTH hold:
#   1. one of M's parents U is an ancestor of the live <upstream-ref>, and
#      M's own blob for the path equals U's blob for the path exactly (the
#      merge took upstream's content for this path wholesale) -- this is
#      what proves the resulting content is genuinely upstream-derived, not
#      Midad's own prior edit merely surviving the merge unresolved; and
#   2. at least one of M's OTHER parents has a DIFFERENT blob for the path
#      (the merge was non-trivial for this path -- it actually changed
#      something). Without this, a path M never touched at all would
#      "accidentally qualify" any time its content coincidentally matches
#      across parents, mislabeling an ordinary untouched file.
# Requirement 1 is also what closes a subtler attack: if Midad had ALREADY
# diverged a path before a later real merge, that merge's own blob for the
# path would need to differ from its own upstream parent's blob (since the
# prior divergence survived) -- which fails requirement 1 and correctly
# refuses to launder that prior divergence into a trusted baseline.
#
# The most recent (first found, since `git log` defaults to reverse-
# chronological order) qualifying merge wins. If none is found, the path
# behaves exactly as before this section existed -- this only ever adds a
# new PASS state; it never changes what already failed.
#
# --- Reviewed upstream backport (exact-content exception) -------------------
# Baseline-awareness alone does not, and must not, pass a path whose HEAD
# content is genuinely new relative to its adopted baseline (or plain
# BASE_REF, for a path with no real-merge history) -- that is still real,
# first-time Midad-side divergence and stays gated. scripts/reviewed-
# upstream-backports.tsv authorizes exactly one further, narrow exception
# for that remaining case: HEAD matches the EXACT byte-for-byte result of
# applying one specific, named, already-accepted official CrossPoint
# commit's ISOLATED per-path patch to the effective base blob (the adopted
# baseline above when one exists, else BASE_REF's own blob). It is NOT an
# allowlist for a path, a permanent per-file exemption, or a way to approve
# a range of commits -- one extra Midad-authored line, or the wrong upstream
# SHA, or a target commit that also touches unrelated content, each cause it
# to stop matching and fall back to an ordinary, gated new_divergence.
#
# Trust boundary: the manifest is read via `git show "$BASE_REF:..."` --
# BASE_REF, never HEAD_REF -- so an ordinary PR editing its own copy has no
# effect on what this run of the guard trusts. The manifest is also listed
# in SECURITY_BOUNDARY_FILES below, so an ordinary PR cannot edit it at all;
# changing it requires the same human security-boundary maintenance
# procedure as thin-fork-guard.sh itself (docs/upstream-sync-architecture.md).
# See verify_reviewed_backport() below for the exact 7-point verification
# (manifest record exists; recorded base/head blobs match exactly; the named
# commit resolves, is a single-parent commit, is an ancestor of live
# <upstream-ref>, and touches this path; and applying that commit's isolated
# per-path patch to the recorded base blob reproduces the recorded head blob
# byte-for-byte via `git apply` + `git hash-object` -- step 7, not step 6, is
# what pins the exact content (a commit can legitimately touch other files
# too; step 6 alone doesn't rule that out, and doesn't need to), never a
# "contains a subset of the changed lines" check).
#
# Requires measure-conflicts.sh in the same directory. Both refs and
# <upstream-ref> must already be resolvable (fetched) by the caller.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BASE_REF="${1:?usage: thin-fork-guard.sh <base-ref> <head-ref> [upstream-ref] [head-branch-name]}"
HEAD_REF="${2:?usage: thin-fork-guard.sh <base-ref> <head-ref> [upstream-ref] [head-branch-name]}"
UPSTREAM_REF="${3:-origin/upstream}"
HEAD_BRANCH_NAME="${4:-}"

COMMON_REF="$(git merge-base "$BASE_REF" "$UPSTREAM_REF")" || {
  echo "thin-fork-guard.sh: no common ancestor between '$BASE_REF' and '$UPSTREAM_REF' -- cannot reason about upstream divergence" >&2
  exit 1
}

# --- Sync-PR ancestry check --------------------------------------------------
# Branch name alone (sync/crosspoint-*) is never trusted -- a feature branch
# renamed to that prefix must not be treated as a sync PR. Recognition
# requires a real merge commit unique to <head-ref> whose parent is (an
# ancestor of) <upstream-ref>'s current tip AND whose message matches
# update-from-crosspoint.yml's exact generated pattern
# ("Merge crosspoint-reader/<ref> @ <sha>"). Run unconditionally, regardless
# of branch name -- name is only used below for a mismatch warning. Recognition
# does NOT disable any gate below (see header comment).
is_recognized_sync_pr() {
  local base="$1" head="$2" upstream="$3"
  git merge-base --is-ancestor "$upstream" "$head" 2>/dev/null || return 1

  local line commit parents p
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    commit="${line%% *}"
    parents="${line#* }"
    for p in $parents; do
      if git merge-base --is-ancestor "$p" "$upstream" 2>/dev/null; then
        if git log -1 --format=%s "$commit" | grep -q '^Merge crosspoint-reader/'; then
          return 0
        fi
      fi
    done
  done < <(git log --merges --format='%H %P' "$base..$head" 2>/dev/null)
  return 1
}

# --- 1. Conflict counts, before vs. after ------------------------------------
BASE_CONFLICTS_FILE="$(mktemp)"
HEAD_CONFLICTS_FILE="$(mktemp)"
BASE_DIVERGED_FILE="$(mktemp)"
HEAD_DIVERGED_FILE="$(mktemp)"
trap 'rm -f "$BASE_CONFLICTS_FILE" "$HEAD_CONFLICTS_FILE" "$BASE_DIVERGED_FILE" "$HEAD_DIVERGED_FILE"' EXIT

# Not wrapped in an if/&&/|| -- a measure-conflicts.sh failure that is NOT a
# content conflict (see that script's header) must abort this script under
# `set -e` rather than being caught and treated as "0 conflicts."
"$SCRIPT_DIR/measure-conflicts.sh" "$BASE_REF" "$UPSTREAM_REF" >"$BASE_CONFLICTS_FILE"
"$SCRIPT_DIR/measure-conflicts.sh" "$HEAD_REF" "$UPSTREAM_REF" >"$HEAD_CONFLICTS_FILE"

BASE_COUNT=$(grep -c . "$BASE_CONFLICTS_FILE" || true)
HEAD_COUNT=$(grep -c . "$HEAD_CONFLICTS_FILE" || true)

NEW_CONFLICTS="$(comm -13 "$BASE_CONFLICTS_FILE" "$HEAD_CONFLICTS_FILE")"
RESOLVED_CONFLICTS="$(comm -23 "$BASE_CONFLICTS_FILE" "$HEAD_CONFLICTS_FILE")"

# --- 2. Shared/upstream-owned files this PR's own diff touches --------------
# A path counts as having upstream lineage -- "shared" -- if it exists
# EITHER at COMMON_REF (the merge-base computed above) OR at UPSTREAM_REF's
# current tip. Checking only "exists in current UPSTREAM_REF" (the original,
# simpler check) silently loses ownership for a path upstream has since
# deleted or renamed away: Midad may still be carrying that path, and its
# shared history with upstream still matters for divergence purposes. The
# UPSTREAM_REF arm alone still catches a path upstream created after
# COMMON_REF, so nothing regresses relative to the original check.
is_shared_path() {
  local path="$1"
  if git cat-file -e "$COMMON_REF:$path" 2>/dev/null; then
    return 0
  fi
  if git cat-file -e "$UPSTREAM_REF:$path" 2>/dev/null; then
    return 0
  fi
  return 1
}

PR_DIFF_FILES="$(git diff --name-only "$BASE_REF...$HEAD_REF")"

UPSTREAM_TOUCHED=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  if is_shared_path "$path"; then
    UPSTREAM_TOUCHED="${UPSTREAM_TOUCHED}${path}"$'\n'
  fi
done <<<"$PR_DIFF_FILES"

# Renames away from a shared path: ownership-by-current-path stops tracking
# these going forward, and a plain `git diff --name-only` (what
# PR_DIFF_FILES/UPSTREAM_TOUCHED above is built from) does not surface the
# old path for a detected rename at all -- only --name-status does. A
# rename of a path that was Midad-clean relative to COMMON_REF (i.e. Midad
# had not yet touched it) is functionally the first Midad-side divergence
# for that content, regardless of whether the renamed file's bytes changed:
# the path itself is what lets a file keep tracking upstream, and the old
# path is simply gone from HEAD after a rename. Content equality doesn't
# rescue this -- a byte-for-byte `git mv` still severs upstream tracking.
# A rename of a path that was ALREADY Midad-diverged before this PR carries
# no new information the guard needs to block; report only, same as any
# other pre-existing divergence. See CLAUDE.md/docs/upstream-sync-architecture.md.
RENAME_NEW_DIVERGED=""
RENAME_ALREADY_DIVERGED=""
while IFS=$'\t' read -r _ old_path new_path; do
  [ -z "$old_path" ] && continue
  if is_shared_path "$old_path"; then
    common_old_sha="$(git rev-parse -q --verify "$COMMON_REF:$old_path" 2>/dev/null || true)"
    base_old_sha="$(git rev-parse -q --verify "$BASE_REF:$old_path" 2>/dev/null || true)"
    if [ "$base_old_sha" = "$common_old_sha" ]; then
      RENAME_NEW_DIVERGED="${RENAME_NEW_DIVERGED}\`${old_path}\` -> \`${new_path}\`"$'\n'
    else
      RENAME_ALREADY_DIVERGED="${RENAME_ALREADY_DIVERGED}\`${old_path}\` -> \`${new_path}\`"$'\n'
    fi
  fi
done < <(git diff --name-status --diff-filter=R "$BASE_REF...$HEAD_REF")

# Deletions of shared files: reported explicitly, not folded silently into
# "no longer touches upstream files." Also flows through the Midad-side
# divergence classification below like any other content change (a
# deletion is just "HEAD's blob is empty"), so a deletion of a previously
# Midad-clean file correctly fails there without any special-casing here.
DELETE_WARNINGS=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  if is_shared_path "$path"; then
    DELETE_WARNINGS="${DELETE_WARNINGS}${path}"$'\n'
  fi
done < <(git diff --name-only --diff-filter=D "$BASE_REF...$HEAD_REF")

# --- 3a. Divergence vs. current upstream tip (informational only) -----------
# Simple two-state comparison: does this path's blob at <ref> differ from
# its blob at UPSTREAM_REF, right now. Kept and reported because it answers
# a genuinely different, still-useful question -- how far a file has
# drifted from upstream *today*, regardless of who caused it or when -- but
# per the header comment above, this is NOT used to decide FAIL/PASS.
compute_diverged_files() {
  local ref="$1"
  local path ref_sha upstream_sha
  while IFS= read -r path; do
    [ -z "$path" ] && continue
    ref_sha="$(git rev-parse -q --verify "$ref:$path" 2>/dev/null || true)"
    upstream_sha="$(git rev-parse -q --verify "$UPSTREAM_REF:$path" 2>/dev/null || true)"
    if [ "$ref_sha" != "$upstream_sha" ]; then
      echo "$path"
    fi
  done <<<"$UPSTREAM_TOUCHED" | sort
}

compute_diverged_files "$BASE_REF" >"$BASE_DIVERGED_FILE"
compute_diverged_files "$HEAD_REF" >"$HEAD_DIVERGED_FILE"

NEW_DIVERGED="$(comm -13 "$BASE_DIVERGED_FILE" "$HEAD_DIVERGED_FILE")"
RESOLVED_DIVERGED="$(comm -23 "$BASE_DIVERGED_FILE" "$HEAD_DIVERGED_FILE")"
ALREADY_DIVERGED="$(comm -12 "$BASE_DIVERGED_FILE" "$HEAD_DIVERGED_FILE")"

# --- 3a-2. Reviewed-upstream-backport manifest (trust boundary: BASE_REF only) ---
# See the header comment's "Reviewed upstream backport" section for the full
# design. Format: TSV, one record per line, `#`-prefixed and blank lines
# ignored:
#   PATH<TAB>UPSTREAM_COMMIT_SHA<TAB>BASE_BLOB_SHA<TAB>APPROVED_HEAD_BLOB_SHA[<TAB>NOTE]
# BASE_BLOB_SHA means the EFFECTIVE base for that path -- the path's
# adopted-upstream blob (find_adopted_upstream_blob above) when one exists,
# otherwise BASE_REF's own blob for that path; verify_reviewed_backport
# below is always called with whichever one actually applies. Any
# non-comment, non-blank line missing one of the first four fields, or any
# duplicate PATH, is a malformed manifest -- fails the whole run closed
# (checked immediately below), the same posture as the workflow-permission
# audit's WORKFLOW_AUDIT_ERROR: a manifest that fails to parse must never
# silently be read as "zero backport records."
MANIFEST_PATH="scripts/reviewed-upstream-backports.tsv"
REVIEWED_BACKPORT_PATHS=()
REVIEWED_BACKPORT_UPSTREAM_SHAS=()
REVIEWED_BACKPORT_BASE_BLOBS=()
REVIEWED_BACKPORT_HEAD_BLOBS=()
REVIEWED_BACKPORT_NOTES=()
REVIEWED_BACKPORT_MANIFEST_ERROR=""

load_reviewed_backport_manifest() {
  if ! git cat-file -e "$BASE_REF:$MANIFEST_PATH" 2>/dev/null; then
    return 0 # no manifest at BASE_REF yet -- zero records, not an error
  fi
  local content
  if ! content="$(git show "$BASE_REF:$MANIFEST_PATH" 2>&1)"; then
    REVIEWED_BACKPORT_MANIFEST_ERROR="failed to read '$MANIFEST_PATH' at '$BASE_REF': $content"
    return 1
  fi
  local line lineno=0 f_path f_upstream f_base f_head f_note seen="|"
  while IFS= read -r line; do
    lineno=$((lineno + 1))
    [ -z "$line" ] && continue
    case "$line" in '#'*) continue ;; esac
    IFS=$'\t' read -r f_path f_upstream f_base f_head f_note <<<"$line"
    if [ -z "$f_path" ] || [ -z "$f_upstream" ] || [ -z "$f_base" ] || [ -z "$f_head" ]; then
      REVIEWED_BACKPORT_MANIFEST_ERROR="malformed manifest line $lineno (need PATH<TAB>UPSTREAM_COMMIT_SHA<TAB>BASE_BLOB_SHA<TAB>APPROVED_HEAD_BLOB_SHA[<TAB>NOTE]): $line"
      return 1
    fi
    case "$seen" in
      *"|${f_path}|"*)
        REVIEWED_BACKPORT_MANIFEST_ERROR="malformed manifest: duplicate record for path '$f_path' (line $lineno)"
        return 1
        ;;
    esac
    seen="${seen}${f_path}|"
    REVIEWED_BACKPORT_PATHS+=("$f_path")
    REVIEWED_BACKPORT_UPSTREAM_SHAS+=("$f_upstream")
    REVIEWED_BACKPORT_BASE_BLOBS+=("$f_base")
    REVIEWED_BACKPORT_HEAD_BLOBS+=("$f_head")
    REVIEWED_BACKPORT_NOTES+=("$f_note")
  done <<<"$content"
  return 0
}

if ! load_reviewed_backport_manifest; then
  echo "thin-fork-guard.sh: reviewed-upstream-backports manifest is malformed -- failing closed:" >&2
  echo "$REVIEWED_BACKPORT_MANIFEST_ERROR" >&2
  exit 1
fi

# lookup_reviewed_backport_record <path>: sets RB_UPSTREAM_SHA/RB_BASE_BLOB/
# RB_HEAD_BLOB/RB_NOTE and returns 0 if a manifest record exists for <path>;
# returns 1 (nothing set) otherwise.
lookup_reviewed_backport_record() {
  local search_path="$1" i
  for ((i = 0; i < ${#REVIEWED_BACKPORT_PATHS[@]}; i++)); do
    if [ "${REVIEWED_BACKPORT_PATHS[$i]}" = "$search_path" ]; then
      RB_UPSTREAM_SHA="${REVIEWED_BACKPORT_UPSTREAM_SHAS[$i]}"
      RB_BASE_BLOB="${REVIEWED_BACKPORT_BASE_BLOBS[$i]}"
      RB_HEAD_BLOB="${REVIEWED_BACKPORT_HEAD_BLOBS[$i]}"
      RB_NOTE="${REVIEWED_BACKPORT_NOTES[$i]}"
      return 0
    fi
  done
  return 1
}

# verify_reviewed_backport <path> <effective_base_sha> <head_sha>: returns 0
# only if ALL of the following hold (see the header comment's "Reviewed
# upstream backport" section):
#   1. an exact manifest record exists for <path>
#   2. <effective_base_sha> equals the record's BASE_BLOB_SHA
#   3. <head_sha> equals the record's APPROVED_HEAD_BLOB_SHA
#   4. the record's UPSTREAM_COMMIT_SHA resolves to a real commit (present
#      because the caller fetched live upstream before this script ran)
#   5. that commit is a single-parent (non-merge) commit and an ancestor of
#      the freshly fetched UPSTREAM_REF
#   6. that commit's own diff touches <path> (the commit may also touch
#      other files -- this step alone does not rule that out, and doesn't
#      need to; step 7 is what pins the exact content)
#   7. applying that commit's ISOLATED diff for <path> (not the whole
#      commit) to the recorded base blob reproduces APPROVED_HEAD_BLOB_SHA
#      byte-for-byte
# Any failure at any step -- including a git/patch-apply error that isn't a
# clean "no" -- returns 1 (fail closed). This is deliberately NOT "contains
# a subset of the named commit's changed lines"; #7 is exact-content
# verification via git apply + git hash-object, so an unrelated upstream
# commit touching the same file (e.g. CrossPoint's bbca4886 alongside the
# accepted 63fcbab02c99fc61969c65c25c539c021c12ece5) cannot smuggle its own
# changes in under cover of a narrow, isolated, already-reviewed backport.
verify_reviewed_backport() {
  local path="$1" effective_base_sha="$2" head_sha="$3"
  local upstream_sha approved_base approved_head

  lookup_reviewed_backport_record "$path" || return 1
  upstream_sha="$RB_UPSTREAM_SHA"
  approved_base="$RB_BASE_BLOB"
  approved_head="$RB_HEAD_BLOB"

  [ "$effective_base_sha" = "$approved_base" ] || return 1
  [ "$head_sha" = "$approved_head" ] || return 1
  git cat-file -e "${upstream_sha}^{commit}" 2>/dev/null || return 1

  local parents
  parents="$(git rev-list --parents -n1 "$upstream_sha" 2>/dev/null)" || return 1
  [ "$(wc -w <<<"$parents")" -eq 2 ] || return 1

  git merge-base --is-ancestor "$upstream_sha" "$UPSTREAM_REF" 2>/dev/null || return 1

  local touched
  touched="$(git diff --name-only "${upstream_sha}^" "$upstream_sha" -- "$path" 2>/dev/null)" || return 1
  [ -n "$touched" ] || return 1

  local work base_content patch_file result_sha apply_status
  work="$(mktemp -d)" || return 1
  base_content="$work/base_blob"
  patch_file="$work/patch.diff"

  if ! git cat-file blob "$approved_base" >"$base_content" 2>/dev/null; then
    rm -rf "$work"
    return 1
  fi
  if ! git diff "${upstream_sha}^" "$upstream_sha" -- "$path" >"$patch_file" 2>/dev/null; then
    rm -rf "$work"
    return 1
  fi

  (
    set +e
    cd "$work" &&
      git init -q . &&
      mkdir -p "$(dirname "$path")" &&
      cp "$base_content" "$path" &&
      git apply --check "$patch_file" 2>/dev/null &&
      git apply "$patch_file" 2>/dev/null
  )
  apply_status=$?
  if [ "$apply_status" -ne 0 ]; then
    rm -rf "$work"
    return 1
  fi

  result_sha="$(git hash-object "$work/$path" 2>/dev/null)" || {
    rm -rf "$work"
    return 1
  }
  rm -rf "$work"
  [ "$result_sha" = "$approved_head" ]
}

# --- 3b. Midad-side divergence vs. COMMON_REF (authoritative gate) ----------
# classify_divergence <common_sha> <base_sha> <head_sha> <upstream_sha> <adopted_sha>
# Echoes one of: clean | converged | adopted_baseline | new_divergence | already_diverged.
# See the header comment's four-state table for the exact rule; convergence
# (HEAD == UPSTREAM) is checked first and wins regardless of BASE's prior
# state, since adopting upstream's exact current content is always good
# news, including when it resolves a pre-existing divergence. adopted_baseline
# (HEAD == the path's own real-merge-derived adopted blob, see
# find_adopted_upstream_blob below) is checked next, for the same reason:
# matching what this PR itself genuinely, verifiably merged from upstream is
# good news independent of where live upstream has since drifted to.
# <adopted_sha> may be empty (no qualifying real merge found for this path),
# in which case this check is skipped and behavior is unchanged from before
# this state existed.
classify_divergence() {
  local common="$1" base="$2" head="$3" upstream="$4" adopted="$5"
  if [ "$head" = "$upstream" ]; then
    echo "converged"
    return
  fi
  if [ -n "$adopted" ] && [ "$head" = "$adopted" ]; then
    echo "adopted_baseline"
    return
  fi
  if [ "$base" = "$common" ]; then
    if [ "$head" = "$base" ]; then
      echo "clean"
    else
      echo "new_divergence"
    fi
  else
    echo "already_diverged"
  fi
}

# All real merge commits in base..head, cached once (not per-path) as
# "<commit> <parent> [<parent> ...]" lines -- find_adopted_upstream_blob
# consumes this; computing it once avoids re-running `git log --merges` for
# every shared path.
MERGE_COMMITS_IN_RANGE="$(git log --merges --format='%H %P' "$BASE_REF..$HEAD_REF" 2>/dev/null || true)"

# Parents of in-range merge commits that are ancestors of the live
# <upstream-ref>, computed once. This decision is path-independent (it
# depends only on the commit graph, not on any single path's blob), so
# find_adopted_upstream_blob would otherwise fork one `git merge-base
# --is-ancestor` per (path, parent) pair -- for a thin fork where
# UPSTREAM_TOUCHED can hold every upstream-owned path, that's a lot of
# identical, wasted subprocess calls. `|`-delimited membership string,
# matching the style already used for GOVERNANCE_FILES/SECURITY_BOUNDARY_FILES
# membership checks elsewhere in this script.
UPSTREAM_ANCESTOR_PARENTS="|"
while IFS= read -r _mc_line; do
  [ -z "$_mc_line" ] && continue
  for _mc_p in ${_mc_line#* }; do
    case "$UPSTREAM_ANCESTOR_PARENTS" in *"|${_mc_p}|"*) continue ;; esac
    if git merge-base --is-ancestor "$_mc_p" "$UPSTREAM_REF" 2>/dev/null; then
      UPSTREAM_ANCESTOR_PARENTS="${UPSTREAM_ANCESTOR_PARENTS}${_mc_p}|"
    fi
  done
done <<<"$MERGE_COMMITS_IN_RANGE"

# find_adopted_upstream_blob <path>: echoes the path's adopted-upstream blob
# SHA and returns 0 if this PR's own history contains a qualifying real
# merge for it (see the header comment's "Baseline-aware classification"
# section for the exact two-part qualification rule); returns 1 (nothing
# echoed) otherwise.
find_adopted_upstream_blob() {
  local path="$1"
  local line commit parents p m_blob p_blob other_differs
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    commit="${line%% *}"
    parents="${line#* }"
    m_blob="$(git rev-parse -q --verify "$commit:$path" 2>/dev/null || true)"
    [ -z "$m_blob" ] && continue
    other_differs=false
    for p in $parents; do
      p_blob="$(git rev-parse -q --verify "$p:$path" 2>/dev/null || true)"
      [ "$p_blob" != "$m_blob" ] && other_differs=true
    done
    # Only a genuine adoption event if the merge actually changed this path
    # relative to at least one parent -- otherwise the blob match is
    # coincidental (e.g. a path neither side ever touched), not evidence of
    # a real-merge-driven adoption.
    [ "$other_differs" = true ] || continue
    for p in $parents; do
      case "$UPSTREAM_ANCESTOR_PARENTS" in *"|${p}|"*) ;; *) continue ;; esac
      p_blob="$(git rev-parse -q --verify "$p:$path" 2>/dev/null || true)"
      if [ -n "$p_blob" ] && [ "$m_blob" = "$p_blob" ]; then
        echo "$m_blob"
        return 0
      fi
    done
  done <<<"$MERGE_COMMITS_IN_RANGE"
  return 1
}

MIDAD_NEW_DIVERGED=""
MIDAD_CONVERGED=""
MIDAD_ALREADY_DIVERGED=""
MIDAD_ADOPTED_BASELINE=""
MIDAD_REVIEWED_BACKPORT=""
MIDAD_REVIEWED_BACKPORT_DETAIL=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  common_sha="$(git rev-parse -q --verify "$COMMON_REF:$path" 2>/dev/null || true)"
  base_sha="$(git rev-parse -q --verify "$BASE_REF:$path" 2>/dev/null || true)"
  head_sha="$(git rev-parse -q --verify "$HEAD_REF:$path" 2>/dev/null || true)"
  upstream_sha="$(git rev-parse -q --verify "$UPSTREAM_REF:$path" 2>/dev/null || true)"
  adopted_sha="$(find_adopted_upstream_blob "$path" || true)"
  case "$(classify_divergence "$common_sha" "$base_sha" "$head_sha" "$upstream_sha" "$adopted_sha")" in
    new_divergence)
      effective_base_sha="${adopted_sha:-$base_sha}"
      if verify_reviewed_backport "$path" "$effective_base_sha" "$head_sha"; then
        MIDAD_REVIEWED_BACKPORT="${MIDAD_REVIEWED_BACKPORT}${path}"$'\n'
        MIDAD_REVIEWED_BACKPORT_DETAIL="${MIDAD_REVIEWED_BACKPORT_DETAIL}${path} <- CrossPoint ${RB_UPSTREAM_SHA}${RB_NOTE:+ (${RB_NOTE})}"$'\n'
      else
        MIDAD_NEW_DIVERGED="${MIDAD_NEW_DIVERGED}${path}"$'\n'
      fi
      ;;
    converged) MIDAD_CONVERGED="${MIDAD_CONVERGED}${path}"$'\n' ;;
    adopted_baseline) MIDAD_ADOPTED_BASELINE="${MIDAD_ADOPTED_BASELINE}${path}"$'\n' ;;
    already_diverged) MIDAD_ALREADY_DIVERGED="${MIDAD_ALREADY_DIVERGED}${path}"$'\n' ;;
    clean) : ;;
  esac
done <<<"$UPSTREAM_TOUCHED"

# --- 3c. Stale reviewed-backport manifest entries (informational only) ------
# A manifest record becomes obsolete the moment live upstream's CURRENT tip
# blob for that path equals the record's own APPROVED_HEAD_BLOB_SHA -- at
# that point classify_divergence's HEAD==UPSTREAM check already resolves the
# path as ordinary "converged" (see its "always wins" comment above), so the
# backport record is no longer doing any work. Reported so the manifest can
# be cleaned up; never fails a PR on its own.
STALE_REVIEWED_BACKPORTS=""
for ((rb_i = 0; rb_i < ${#REVIEWED_BACKPORT_PATHS[@]}; rb_i++)); do
  rb_path="${REVIEWED_BACKPORT_PATHS[$rb_i]}"
  rb_approved_head="${REVIEWED_BACKPORT_HEAD_BLOBS[$rb_i]}"
  rb_upstream_now="$(git rev-parse -q --verify "$UPSTREAM_REF:$rb_path" 2>/dev/null || true)"
  if [ -n "$rb_upstream_now" ] && [ "$rb_upstream_now" = "$rb_approved_head" ]; then
    STALE_REVIEWED_BACKPORTS="${STALE_REVIEWED_BACKPORTS}${rb_path}"$'\n'
  fi
done

# Architecture-governance files: no branch-protection review requirement is
# possible on a solo-owner repo (see thin-fork-guard PR's own description
# for the CODEOWNERS trade-off), so this is the guard's substitute --
# purely informational, never blocking, just making sure the one reviewer
# never merges a change to these without noticing.
GOVERNANCE_FILES=(
  ".skills/SKILL.md"
  "docs/upstream-sync-architecture.md"
  ".github/workflows/update-from-crosspoint.yml"
  ".github/workflows/thin-fork-guard.yml"
  ".github/workflows/thin-fork-guard-trusted.yml"
  ".github/workflows/ci.yml"
  "scripts/thin-fork-guard.sh"
  "scripts/measure-conflicts.sh"
  "scripts/check-workflow-permissions.rb"
  "scripts/test-thin-fork-guard.sh"
  "scripts/reviewed-upstream-backports.tsv"
)
GOVERNANCE_TOUCHED=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  for gf in "${GOVERNANCE_FILES[@]}"; do
    if [ "$path" = "$gf" ]; then
      GOVERNANCE_TOUCHED="${GOVERNANCE_TOUCHED}${path}"$'\n'
    fi
  done
done <<<"$PR_DIFF_FILES"

# --- 6. Security-boundary files (independent hard gate) ----------------------
# These files define or influence enforcement itself: the trusted
# evaluator's own workflow, the CI entrypoint, and this guard's own
# scripts (including the permission-audit parser section 7 depends on --
# a PR that could silently neuter check-workflow-permissions.rb would
# defeat gate 5 just as completely as editing thin-fork-guard.sh itself).
# Independent of upstream ownership, existing divergence, or conflict
# count -- none of those concepts capture "this PR is touching the
# machinery that decides whether PRs pass." An ordinary PR must never
# modify, delete, or rename these; see docs/upstream-sync-architecture.md's
# "Security-boundary file maintenance" section for the explicit human
# procedure required to change one legitimately (relax the required check,
# merge, restore, re-validate).
SECURITY_BOUNDARY_FILES=(
  ".github/workflows/thin-fork-guard-trusted.yml"
  ".github/workflows/ci.yml"
  ".github/workflows/update-from-crosspoint.yml"
  "scripts/thin-fork-guard.sh"
  "scripts/measure-conflicts.sh"
  "scripts/check-workflow-permissions.rb"
  "scripts/test-thin-fork-guard.sh"
  "scripts/reviewed-upstream-backports.tsv"
)
SECURITY_BOUNDARY_TOUCHED=""
# Plain modify/delete: PR_DIFF_FILES (git diff --name-only) reliably lists
# both, so a straight membership check is sufficient here.
while IFS= read -r path; do
  [ -z "$path" ] && continue
  for sb in "${SECURITY_BOUNDARY_FILES[@]}"; do
    if [ "$path" = "$sb" ]; then
      SECURITY_BOUNDARY_TOUCHED="${SECURITY_BOUNDARY_TOUCHED}${path} (modified/deleted)"$'\n'
    fi
  done
done <<<"$PR_DIFF_FILES"
# Renames: a plain `git diff --name-only` does not reliably surface a
# detected rename's OLD path at all (see is_shared_path's note above) --
# only `--name-status` does. Check BOTH the source and destination path of
# every rename against the boundary list: a rename AWAY from a boundary
# path is functionally a deletion of it, and a rename INTO a boundary
# pathname is functionally a modification of it (whatever new content
# just landed at that path is now what "the trusted workflow" or "the
# guard script" means going forward) -- both must fail, independent of
# the general thin-fork rename gate above, which only cares about
# upstream-clean shared files.
while IFS=$'\t' read -r _ rn_old_path rn_new_path; do
  [ -z "$rn_old_path" ] && continue
  for sb in "${SECURITY_BOUNDARY_FILES[@]}"; do
    if [ "$rn_old_path" = "$sb" ] || [ "$rn_new_path" = "$sb" ]; then
      SECURITY_BOUNDARY_TOUCHED="${SECURITY_BOUNDARY_TOUCHED}\`${rn_old_path}\` -> \`${rn_new_path}\` (rename)"$'\n'
    fi
  done
done < <(git diff --name-status --diff-filter=R "$BASE_REF...$HEAD_REF")

# --- 7. Workflow-permission audit (independent hard gate) --------------------
# One invariant: no ordinary PR may create another workflow capable of
# spoofing the trusted required status/check. Parses every workflow
# file's CONTENT at HEAD_REF (via `git show`, git plumbing -- never
# executed, consistent with this guard never running anything from HEAD)
# as YAML via scripts/check-workflow-permissions.rb, and inspects the
# ACTUAL RESOLVED permission values (workflow-level and every
# jobs.<job>.permissions block) -- not a text/regex match against one
# exact spelling. A regex for the literal bytes `statuses: write` is
# defeated by any equivalent-meaning YAML spelling (`statuses: "write"`,
# extra spacing, a YAML alias resolving to the string "write"), all of
# which are ordinary valid YAML, not exotic bypasses; only reading the
# document's actual semantics catches all of them. See that script's own
# header for why Ruby's bundled Psych (YAML.safe_load) is the parser: part
# of Ruby's standard library (no gem/network fetch), Ruby itself is part
# of GitHub's ubuntu-latest runner image (no setup step), and safe_load
# restricts deserialization to plain data -- appropriate for parsing
# untrusted PR content without executing it.
#
# Fails CLOSED, not open: enumerating workflow files, reading one via
# `git show`, or parsing one as YAML can each fail for reasons that have
# nothing to do with whether it contains a forbidden permission (a
# transient git error, a workflow file that's simply not valid YAML).
# Silently treating any of those as "no violation found" would be exactly
# backwards for a security gate -- WORKFLOW_AUDIT_ERROR captures any such
# failure and aborts the whole script immediately (see below), the same
# fail-closed posture COMMON_REF and measure-conflicts.sh already use.
CHECK_PERMS_SCRIPT="$SCRIPT_DIR/check-workflow-permissions.rb"
WORKFLOW_PERMISSION_VIOLATIONS=""
WORKFLOW_AUDIT_ERROR=""

WORKFLOW_FILES_AT_HEAD=""
# `git ls-tree`'s own exit status is captured directly (not through a pipe
# to grep) so it can't be conflated with grep's exit 1 for "no lines
# matched" -- under `set -e -o pipefail`, a piped command's combined exit
# status doesn't distinguish "the git call itself failed" from "it
# succeeded with zero matching filenames," and treating the latter as an
# error would make an ordinary PR that never touches .github/workflows/ at
# all fail closed for no reason.
set +e
workflow_ls_output="$(git ls-tree -r --name-only "$HEAD_REF" -- .github/workflows/ 2>&1)"
workflow_ls_status=$?
set -e
if [ "$workflow_ls_status" -ne 0 ]; then
  WORKFLOW_AUDIT_ERROR="failed to enumerate workflow files at '$HEAD_REF': $workflow_ls_output"
else
  WORKFLOW_FILES_AT_HEAD="$(grep -E '\.ya?ml$' <<<"$workflow_ls_output" || true)"
fi

if [ -z "$WORKFLOW_AUDIT_ERROR" ]; then
  while IFS= read -r path; do
    [ -z "$path" ] && continue
    [ "$path" = ".github/workflows/thin-fork-guard-trusted.yml" ] && continue

    if ! content="$(git show "$HEAD_REF:$path" 2>&1)"; then
      WORKFLOW_AUDIT_ERROR="${WORKFLOW_AUDIT_ERROR}failed to read '$path' at '$HEAD_REF': $content"$'\n'
      continue
    fi

    # ruby exits 1 for an ordinary, fully-expected "violation found" result
    # (see check-workflow-permissions.rb's own exit-code contract) -- under
    # `set -e -o pipefail`, a bare `VAR="$(cmd)"` assignment whose pipeline
    # ends non-zero aborts the whole script right here otherwise, same
    # pitfall as the `git ls-tree` enumeration above.
    set +e
    perm_output="$(printf '%s' "$content" | ruby "$CHECK_PERMS_SCRIPT" 2>&1)"
    perm_status=$?
    set -e
    case "$perm_status" in
      0) : ;;
      1) WORKFLOW_PERMISSION_VIOLATIONS="${WORKFLOW_PERMISSION_VIOLATIONS}${path}:"$'\n'"${perm_output}"$'\n' ;;
      *) WORKFLOW_AUDIT_ERROR="${WORKFLOW_AUDIT_ERROR}failed to parse '$path' at '$HEAD_REF' (exit $perm_status): $perm_output"$'\n' ;;
    esac
  done <<<"$WORKFLOW_FILES_AT_HEAD"
fi

if [ -n "$WORKFLOW_AUDIT_ERROR" ]; then
  echo "thin-fork-guard.sh: workflow-permission audit could not complete -- failing closed:" >&2
  echo "$WORKFLOW_AUDIT_ERROR" >&2
  exit 1
fi

# --- 8. Sync-PR recognition (informational only -- see header) --------------
IS_SYNC=false
NAME_MISMATCH=false
if is_recognized_sync_pr "$BASE_REF" "$HEAD_REF" "$UPSTREAM_REF"; then
  IS_SYNC=true
elif [[ "$HEAD_BRANCH_NAME" == sync/crosspoint-* ]]; then
  NAME_MISMATCH=true
fi

# --- 9. Report ---------------------------------------------------------------
{
  echo "## Thin-Fork Guard"
  echo
  echo "Base (\`$BASE_REF\`) conflicting files: **$BASE_COUNT**"
  echo "PR HEAD (\`$HEAD_REF\`) conflicting files: **$HEAD_COUNT**"
  echo "Common ancestor with upstream (\`merge-base $BASE_REF $UPSTREAM_REF\`): \`$COMMON_REF\`"
  echo

  if [ "$IS_SYNC" = true ]; then
    echo "**Recognized genuine CrossPoint sync PR** (verified upstream-merge ancestry from a real \`update-from-crosspoint.yml\` run). This does NOT bypass any gate below -- a genuine sync is expected to pass all of them on their own merits (see docs/upstream-sync-architecture.md's human-review step for what to check if it doesn't)."
    echo
  elif [ "$NAME_MISMATCH" = true ]; then
    echo "**WARNING:** branch name \`$HEAD_BRANCH_NAME\` matches \`sync/crosspoint-*\` but does NOT contain a verified upstream-merge ancestry. Treating as an ordinary feature PR -- the full guard applies below."
    echo
  fi

  if [ -n "$NEW_CONFLICTS" ]; then
    echo "### New conflicting files"
    echo '```'
    echo "$NEW_CONFLICTS"
    echo '```'
  else
    echo "No new conflicting files."
  fi
  echo

  if [ -n "$RESOLVED_CONFLICTS" ]; then
    echo "### Conflicts resolved by this PR"
    echo '```'
    echo "$RESOLVED_CONFLICTS"
    echo '```'
    echo
  fi

  if [ -n "$MIDAD_NEW_DIVERGED" ]; then
    echo "### New Midad-side divergence (gates this PR)"
    echo "These files were Midad-clean (identical to the last history shared with upstream at \`$COMMON_REF\`) before this PR, and are no longer clean at \`$HEAD_REF\`:"
    echo '```'
    echo "$MIDAD_NEW_DIVERGED"
    echo '```'
  else
    echo "No new Midad-side divergence."
  fi
  echo

  if [ -n "$MIDAD_CONVERGED" ]; then
    echo "### Files converged to upstream's current content"
    echo '```'
    echo "$MIDAD_CONVERGED"
    echo '```'
    echo
  fi

  if [ -n "$MIDAD_ADOPTED_BASELINE" ]; then
    echo "### Files matching this PR's own adopted-upstream baseline (not gated)"
    echo "HEAD matches a blob this PR itself genuinely adopted via a real, ancestry-verified merge of upstream -- see find_adopted_upstream_blob's header comment. Live upstream may have moved further since; that is reported separately (below) as drift, not treated as Midad-side divergence:"
    echo '```'
    echo "$MIDAD_ADOPTED_BASELINE"
    echo '```'
    echo
  fi

  if [ -n "$MIDAD_REVIEWED_BACKPORT" ]; then
    echo "### Reviewed upstream backports (exempted from new-divergence gate)"
    echo "Each of these matched an exact, content-verified record in \`scripts/reviewed-upstream-backports.tsv\` (read from \`$BASE_REF\`, never from this PR's own copy):"
    echo '```'
    echo "$MIDAD_REVIEWED_BACKPORT_DETAIL"
    echo '```'
    echo
  fi

  if [ -n "$STALE_REVIEWED_BACKPORTS" ]; then
    echo "### Stale reviewed-backport manifest entries"
    echo "Live upstream's current tip now matches the approved backport content for these paths directly -- ordinary convergence applies, and these \`scripts/reviewed-upstream-backports.tsv\` entries can be removed:"
    echo '```'
    echo "$STALE_REVIEWED_BACKPORTS"
    echo '```'
    echo
  fi

  if [ -n "$MIDAD_ALREADY_DIVERGED" ]; then
    echo "### Pre-existing Midad-side divergence touched (not newly introduced)"
    echo '```'
    echo "$MIDAD_ALREADY_DIVERGED"
    echo '```'
    echo
  fi

  if [ -n "$RENAME_NEW_DIVERGED" ]; then
    echo "### Renames away from a Midad-clean shared path (gates this PR)"
    echo "The source path was Midad-clean relative to \`$COMMON_REF\` before this PR; renaming it away severs upstream tracking the same as deleting it would:"
    echo '```'
    echo "$RENAME_NEW_DIVERGED"
    echo '```'
    echo
  fi

  if [ -n "$RENAME_ALREADY_DIVERGED" ]; then
    echo "### Renames of an already-diverged shared path (not newly diverged)"
    echo '```'
    echo "$RENAME_ALREADY_DIVERGED"
    echo '```'
    echo
  fi

  if [ -n "$NEW_DIVERGED" ]; then
    echo "### Files newly diverged from upstream content (vs. current upstream tip; informational -- see \"New Midad-side divergence\" above for what actually gates this PR)"
    echo '```'
    echo "$NEW_DIVERGED"
    echo '```'
    echo
  fi

  if [ -n "$RESOLVED_DIVERGED" ]; then
    echo "### Files returned to upstream-exact content"
    echo '```'
    echo "$RESOLVED_DIVERGED"
    echo '```'
    echo
  fi

  if [ -n "$ALREADY_DIVERGED" ]; then
    echo "### Already-diverged upstream files touched (vs. current upstream tip; informational)"
    echo '```'
    echo "$ALREADY_DIVERGED"
    echo '```'
    echo
  fi

  echo "### Upstream-owned files touched by this PR"
  if [ -z "$UPSTREAM_TOUCHED" ]; then
    echo "None."
  else
    echo "| File | +/- | Already conflicting (base) | Already diverged vs. current upstream (base) |"
    echo "|---|---|---|---|"
    while IFS= read -r path; do
      [ -z "$path" ] && continue
      stats="$(git diff --numstat "$BASE_REF...$HEAD_REF" -- "$path")"
      add="$(echo "$stats" | awk '{print $1}')"
      del="$(echo "$stats" | awk '{print $2}')"
      if grep -Fxq "$path" "$BASE_CONFLICTS_FILE"; then
        already_conflicting="yes"
      else
        already_conflicting="no"
      fi
      if grep -Fxq "$path" "$BASE_DIVERGED_FILE"; then
        already_diverged="yes"
      else
        already_diverged="no"
      fi
      echo "| \`$path\` | +${add:-0}/-${del:-0} | $already_conflicting | $already_diverged |"
    done <<<"$UPSTREAM_TOUCHED"
  fi
  echo

  if [ -n "$DELETE_WARNINGS" ]; then
    echo "### Upstream-owned files deleted by this PR"
    echo '```'
    echo "$DELETE_WARNINGS"
    echo '```'
    echo
  fi

  if [ -n "$GOVERNANCE_TOUCHED" ]; then
    echo "### Architecture-governance files changed by this PR"
    echo "No enforced review requirement exists for these on this solo-owner repo -- flagged here so they're never merged unnoticed."
    echo '```'
    echo "$GOVERNANCE_TOUCHED"
    echo '```'
    echo
  fi

  if [ -n "$SECURITY_BOUNDARY_TOUCHED" ]; then
    echo "### Security-boundary files touched by this PR (gates this PR)"
    echo "These files define or influence enforcement itself; an ordinary PR must never modify, delete, or rename them (in either direction). See docs/upstream-sync-architecture.md's \"Security-boundary file maintenance\" section for the human procedure required to change one legitimately."
    echo '```'
    echo "$SECURITY_BOUNDARY_TOUCHED"
    echo '```'
    echo
  fi

  if [ -n "$WORKFLOW_PERMISSION_VIOLATIONS" ]; then
    echo "### Workflow files with forbidden status/check-write permissions (gates this PR)"
    echo "Only \`thin-fork-guard-trusted.yml\` may hold these permissions:"
    echo '```'
    echo "$WORKFLOW_PERMISSION_VIOLATIONS"
    echo '```'
    echo
  fi
}

FAIL_REASONS=()
[ -n "$NEW_CONFLICTS" ] && FAIL_REASONS+=("introduces new fixed-upstream merge-conflict surface")
[ -n "$MIDAD_NEW_DIVERGED" ] && FAIL_REASONS+=("introduces new Midad-side divergence in a file that was clean relative to the shared upstream history")
[ -n "$RENAME_NEW_DIVERGED" ] && FAIL_REASONS+=("renames a Midad-clean shared file away from tracking upstream")
[ -n "$SECURITY_BOUNDARY_TOUCHED" ] && FAIL_REASONS+=("security-boundary file modified -- explicit guard-maintenance procedure required")
[ -n "$WORKFLOW_PERMISSION_VIOLATIONS" ] && FAIL_REASONS+=("introduces a workflow with status/check-write permissions outside thin-fork-guard-trusted.yml")

if [ "${#FAIL_REASONS[@]}" -gt 0 ]; then
  echo "FAIL: this PR:"
  for reason in "${FAIL_REASONS[@]}"; do
    echo "  - $reason"
  done
  exit 1
fi

echo "PASS"
exit 0
