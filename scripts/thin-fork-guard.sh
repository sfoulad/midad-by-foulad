#!/usr/bin/env bash
# thin-fork-guard.sh <base-ref> <head-ref> [upstream-ref] [head-branch-name]
#
# Reports, and gates on, three THIN-FORK things <head-ref> may do relative
# to <base-ref>, all measured against the LIVE upstream mirror (default
# origin/upstream, the maintained CrossPoint mirror kept current by
# .github/workflows/update-from-crosspoint.yml), PLUS two independent
# SECURITY-BOUNDARY gates (sections 6-7 below) that protect the guard's own
# enforcement machinery and are not about thin-fork divergence at all:
#
#   1. Introduce new fixed-upstream MERGE-CONFLICT surface (simulated real
#      merge of <upstream-ref> into each side).
#   2. Introduce new MIDAD-SIDE DIVERGENCE in a shared/upstream file that
#      was clean (untouched by Midad) relative to the last history Midad's
#      base actually shared with upstream.
#   3. RENAME a shared file away from its upstream path while that file was
#      still Midad-clean -- a special case of #2 that the general check
#      cannot see on its own (see the note above compute_diverged_files).
#   4. MODIFY a security-boundary file (section 6) -- the trusted workflow,
#      the CI entrypoint, or this guard's own scripts. Independent of
#      upstream ownership, existing divergence, or conflict count: these
#      files define enforcement itself, so any ordinary-PR touch to them
#      fails, full stop. See "Security-boundary file maintenance" in
#      docs/upstream-sync-architecture.md for the explicit human procedure
#      required to change one legitimately.
#   5. INTRODUCE a second status/check-writing workflow (section 7) --
#      `.github/workflows/*.yml` at HEAD other than
#      thin-fork-guard-trusted.yml itself must never contain
#      `statuses: write`, `checks: write`, or `permissions: write-all`.
#      One invariant: no ordinary PR may create another workflow capable of
#      spoofing the trusted required status. A plain text scan of HEAD's
#      workflow file *content* (via `git show`, never executed), not a
#      general YAML security scanner.
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
# Deliberately compares against the LIVE upstream mirror at run time, not a
# hard-coded historical commit: base and head are measured against the
# identical mirror snapshot within one run, so the only variable is the
# PR's own diff -- that stays correct forever, including after the first
# full CrossPoint merge, without ever needing today's number hard-coded.
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

# --- 3b. Midad-side divergence vs. COMMON_REF (authoritative gate) ----------
# classify_divergence <common_sha> <base_sha> <head_sha> <upstream_sha>
# Echoes one of: clean | converged | new_divergence | already_diverged.
# See the header comment's four-state table for the exact rule; convergence
# (HEAD == UPSTREAM) is checked first and wins regardless of BASE's prior
# state, since adopting upstream's exact current content is always good
# news, including when it resolves a pre-existing divergence.
classify_divergence() {
  local common="$1" base="$2" head="$3" upstream="$4"
  if [ "$head" = "$upstream" ]; then
    echo "converged"
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

MIDAD_NEW_DIVERGED=""
MIDAD_CONVERGED=""
MIDAD_ALREADY_DIVERGED=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  common_sha="$(git rev-parse -q --verify "$COMMON_REF:$path" 2>/dev/null || true)"
  base_sha="$(git rev-parse -q --verify "$BASE_REF:$path" 2>/dev/null || true)"
  head_sha="$(git rev-parse -q --verify "$HEAD_REF:$path" 2>/dev/null || true)"
  upstream_sha="$(git rev-parse -q --verify "$UPSTREAM_REF:$path" 2>/dev/null || true)"
  case "$(classify_divergence "$common_sha" "$base_sha" "$head_sha" "$upstream_sha")" in
    new_divergence) MIDAD_NEW_DIVERGED="${MIDAD_NEW_DIVERGED}${path}"$'\n' ;;
    converged) MIDAD_CONVERGED="${MIDAD_CONVERGED}${path}"$'\n' ;;
    already_diverged) MIDAD_ALREADY_DIVERGED="${MIDAD_ALREADY_DIVERGED}${path}"$'\n' ;;
    clean) : ;;
  esac
done <<<"$UPSTREAM_TOUCHED"

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
  "scripts/thin-fork-guard.sh"
  "scripts/measure-conflicts.sh"
  "scripts/test-thin-fork-guard.sh"
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
# scripts. Independent of upstream ownership, existing divergence, or
# conflict count -- none of those concepts capture "this PR is touching the
# machinery that decides whether PRs pass." An ordinary PR must never
# modify these; see docs/upstream-sync-architecture.md's "Security-boundary
# file maintenance" section for the explicit human procedure required to
# change one legitimately (relax the required check, merge, restore,
# re-validate).
SECURITY_BOUNDARY_FILES=(
  ".github/workflows/thin-fork-guard-trusted.yml"
  ".github/workflows/ci.yml"
  ".github/workflows/update-from-crosspoint.yml"
  "scripts/thin-fork-guard.sh"
  "scripts/measure-conflicts.sh"
)
SECURITY_BOUNDARY_TOUCHED=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  for sb in "${SECURITY_BOUNDARY_FILES[@]}"; do
    if [ "$path" = "$sb" ]; then
      SECURITY_BOUNDARY_TOUCHED="${SECURITY_BOUNDARY_TOUCHED}${path}"$'\n'
    fi
  done
done <<<"$PR_DIFF_FILES"

# --- 7. Workflow-permission audit (independent hard gate) --------------------
# One invariant: no ordinary PR may create another workflow capable of
# spoofing the trusted required status/check. Scans every workflow file's
# CONTENT at HEAD_REF (via `git show`, git plumbing -- never executed,
# consistent with this guard never running anything from HEAD) for the
# permission strings that would let a workflow publish or fake a commit
# status/check. thin-fork-guard-trusted.yml itself is exempt here because
# it legitimately needs `statuses: write` -- and it is already covered by
# the security-boundary gate above, so an ordinary PR cannot modify it
# anyway. Deliberately a plain text scan, not a YAML-aware permission
# resolver or general security scanner -- one narrow invariant, kept simple
# and fail-closed.
FORBIDDEN_PERMISSION_PATTERNS=(
  "statuses: write"
  "checks: write"
  "permissions: write-all"
)
WORKFLOW_PERMISSION_VIOLATIONS=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  [ "$path" = ".github/workflows/thin-fork-guard-trusted.yml" ] && continue
  content="$(git show "$HEAD_REF:$path" 2>/dev/null || true)"
  for pattern in "${FORBIDDEN_PERMISSION_PATTERNS[@]}"; do
    if grep -qF "$pattern" <<<"$content"; then
      WORKFLOW_PERMISSION_VIOLATIONS="${WORKFLOW_PERMISSION_VIOLATIONS}${path}: contains \`${pattern}\`"$'\n'
    fi
  done
done < <(git ls-tree -r --name-only "$HEAD_REF" -- .github/workflows/ 2>/dev/null | grep -E '\.ya?ml$')

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
    echo "### Security-boundary files modified by this PR (gates this PR)"
    echo "These files define or influence enforcement itself; an ordinary PR must never modify them. See docs/upstream-sync-architecture.md's \"Security-boundary file maintenance\" section for the human procedure required to change one legitimately."
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
