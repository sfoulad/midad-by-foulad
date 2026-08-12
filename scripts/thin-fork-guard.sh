#!/usr/bin/env bash
# thin-fork-guard.sh <base-ref> <head-ref> [upstream-ref] [head-branch-name]
#
# Reports, and gates on, three things <head-ref> may do relative to
# <base-ref>, all measured against the LIVE upstream mirror (default
# origin/upstream, the maintained CrossPoint mirror kept current by
# .github/workflows/update-from-crosspoint.yml):
#
#   1. Introduce new fixed-upstream MERGE-CONFLICT surface (simulated real
#      merge of <upstream-ref> into each side).
#   2. DIVERGE an upstream-owned file that was byte-identical to upstream
#      at <base-ref> -- i.e. make its content differ from upstream's own
#      copy for the first time, even where that produces no merge conflict
#      today. This matters once <base-ref> already contains the full
#      upstream history as an ancestor (post first full CrossPoint merge):
#      at that point the conflict count for an untouched-but-now-editable
#      upstream file reads 0->0 regardless of what a PR does to it, so
#      conflict-count alone stops being a meaningful gate for files that
#      no longer register as "unmerged" but are still meant to track
#      upstream content exactly.
#   3. RENAME an upstream-owned file that was byte-identical to upstream at
#      <base-ref> away from its upstream path. This is a special case of
#      divergence (#2), not a separate mechanism: the old path simply
#      disappears from <head-ref>, and a plain `git diff --name-only` (what
#      the divergence check's file list is built from) does not surface the
#      old path for a detected rename at all, only `--name-status` does --
#      so without this explicit check, a rename would silently escape both
#      the conflict gate and the divergence gate.
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
# Exit 0: no new conflicting files, no newly-diverged upstream file, and no
#         rename of a previously upstream-exact file away from upstream.
# Exit 1: otherwise, OR if conflict measurement itself fails for a non
#         content-conflict reason (see measure-conflicts.sh -- this script
#         does not catch that failure, so it propagates as a hard abort
#         under `set -e`, which is the fail-closed behavior we want: a
#         broken measurement must never be silently read as "0 conflicts,
#         PASS").
#
# Requires measure-conflicts.sh in the same directory. Both refs and
# <upstream-ref> must already be resolvable (fetched) by the caller.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BASE_REF="${1:?usage: thin-fork-guard.sh <base-ref> <head-ref> [upstream-ref] [head-branch-name]}"
HEAD_REF="${2:?usage: thin-fork-guard.sh <base-ref> <head-ref> [upstream-ref] [head-branch-name]}"
UPSTREAM_REF="${3:-origin/upstream}"
HEAD_BRANCH_NAME="${4:-}"

# --- Sync-PR ancestry check --------------------------------------------------
# Branch name alone (sync/crosspoint-*) is never trusted -- a feature branch
# renamed to that prefix must not be treated as a sync PR. Recognition
# requires a real merge commit unique to <head-ref> whose parent is (an
# ancestor of) <upstream-ref>'s current tip AND whose message matches
# update-from-crosspoint.yml's exact generated pattern
# ("Merge crosspoint-reader/<ref> @ <sha>"). Run unconditionally, regardless
# of branch name -- name is only used below for a mismatch warning. Recognition
# does NOT disable either gate below (see header comment).
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

# --- 2. Upstream-owned files this PR's own diff touches ---------------------
# Ownership: a path is upstream-owned iff it exists at that exact path in
# <upstream-ref>'s tree -- no hand-maintained allowlist. Checked against
# upstream regardless of whether the path currently exists at HEAD, so
# deletions are still correctly classified.
PR_DIFF_FILES="$(git diff --name-only "$BASE_REF...$HEAD_REF")"

UPSTREAM_TOUCHED=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  if git cat-file -e "$UPSTREAM_REF:$path" 2>/dev/null; then
    UPSTREAM_TOUCHED="${UPSTREAM_TOUCHED}${path}"$'\n'
  fi
done <<<"$PR_DIFF_FILES"

# Renames away from an upstream path: ownership-by-current-path stops
# tracking these going forward, and a plain `git diff --name-only` (as used
# to build PR_DIFF_FILES/UPSTREAM_TOUCHED above) does not surface the old
# path for a detected rename at all -- only --name-status does. That means
# the divergence gate below, which only ever looks at UPSTREAM_TOUCHED,
# cannot see a rename on its own: a rename of a path that was upstream-exact
# at base is functionally a deletion of that upstream-exact content (the old
# path is simply gone from HEAD), so it must be treated the same as an
# outright deletion -- FAIL, not a soft "needs human review" note. A rename
# of a path that was ALREADY diverged from upstream at base carries no new
# information the guard needs to block; report only, same as any other
# already-diverged touch. See CLAUDE.md/docs/upstream-sync-architecture.md.
RENAME_NEW_DIVERGED=""
RENAME_ALREADY_DIVERGED=""
while IFS=$'\t' read -r _ old_path new_path; do
  [ -z "$old_path" ] && continue
  if git cat-file -e "$UPSTREAM_REF:$old_path" 2>/dev/null; then
    base_old_sha="$(git rev-parse -q --verify "$BASE_REF:$old_path" 2>/dev/null || true)"
    upstream_old_sha="$(git rev-parse -q --verify "$UPSTREAM_REF:$old_path" 2>/dev/null || true)"
    head_old_sha="$(git rev-parse -q --verify "$HEAD_REF:$old_path" 2>/dev/null || true)"
    if [ "$base_old_sha" = "$upstream_old_sha" ] && [ "$head_old_sha" != "$upstream_old_sha" ]; then
      RENAME_NEW_DIVERGED="${RENAME_NEW_DIVERGED}\`${old_path}\` -> \`${new_path}\`"$'\n'
    else
      RENAME_ALREADY_DIVERGED="${RENAME_ALREADY_DIVERGED}\`${old_path}\` -> \`${new_path}\`"$'\n'
    fi
  fi
done < <(git diff --name-status --diff-filter=R "$BASE_REF...$HEAD_REF")

# Deletions of upstream-owned files: reported explicitly, not folded
# silently into "no longer touches upstream files."
DELETE_WARNINGS=""
while IFS= read -r path; do
  [ -z "$path" ] && continue
  if git cat-file -e "$UPSTREAM_REF:$path" 2>/dev/null; then
    DELETE_WARNINGS="${DELETE_WARNINGS}${path}"$'\n'
  fi
done < <(git diff --name-only --diff-filter=D "$BASE_REF...$HEAD_REF")

# --- 3. Upstream-content divergence, before vs. after ------------------------
# For each upstream-owned path this PR's diff touches, compare the blob SHA
# at BASE_REF/HEAD_REF against the blob SHA at UPSTREAM_REF. Equal SHA (both
# present and identical) = not diverged. Unequal -- including "missing at
# this ref" as a case, via `git rev-parse -q --verify`, empty string on a
# nonexistent path -- = diverged. A file's divergence status can only
# CHANGE between base and head if the path itself is in the PR's own diff
# (PR_DIFF_FILES / UPSTREAM_TOUCHED above); anything outside that diff is
# byte-identical between base and head by definition, so it can't have
# changed status and is correctly excluded from this comparison.
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

# --- 4. Sync-PR recognition (informational only -- see header) --------------
IS_SYNC=false
NAME_MISMATCH=false
if is_recognized_sync_pr "$BASE_REF" "$HEAD_REF" "$UPSTREAM_REF"; then
  IS_SYNC=true
elif [[ "$HEAD_BRANCH_NAME" == sync/crosspoint-* ]]; then
  NAME_MISMATCH=true
fi

# --- 5. Report ---------------------------------------------------------------
{
  echo "## Thin-Fork Guard"
  echo
  echo "Base (\`$BASE_REF\`) conflicting files: **$BASE_COUNT**"
  echo "PR HEAD (\`$HEAD_REF\`) conflicting files: **$HEAD_COUNT**"
  echo

  if [ "$IS_SYNC" = true ]; then
    echo "**Recognized genuine CrossPoint sync PR** (verified upstream-merge ancestry from a real \`update-from-crosspoint.yml\` run). This does NOT bypass either gate below -- a genuine sync is expected to pass both on its own merits (see docs/upstream-sync-architecture.md's human-review step for what to check if it doesn't)."
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

  if [ -n "$NEW_DIVERGED" ]; then
    echo "### Files newly diverged from upstream content"
    echo "Upstream-owned, byte-identical to \`$UPSTREAM_REF\` at \`$BASE_REF\`, no longer identical at \`$HEAD_REF\`:"
    echo '```'
    echo "$NEW_DIVERGED"
    echo '```'
  else
    echo "No files newly diverged from upstream content."
  fi
  echo

  if [ -n "$RESOLVED_DIVERGED" ]; then
    echo "### Files returned to upstream-exact content"
    echo '```'
    echo "$RESOLVED_DIVERGED"
    echo '```'
    echo
  fi

  if [ -n "$ALREADY_DIVERGED" ]; then
    echo "### Already-diverged upstream files touched (not newly diverged)"
    echo '```'
    echo "$ALREADY_DIVERGED"
    echo '```'
    echo
  fi

  echo "### Upstream-owned files touched by this PR"
  if [ -z "$UPSTREAM_TOUCHED" ]; then
    echo "None."
  else
    echo "| File | +/- | Already conflicting (base) | Already diverged (base) |"
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

  if [ -n "$RENAME_NEW_DIVERGED" ]; then
    echo "### Renames away from a previously upstream-exact path (new divergence)"
    echo "The source path was byte-identical to \`$UPSTREAM_REF\` at \`$BASE_REF\`; renaming it away is treated the same as deleting upstream-exact content:"
    echo '```'
    echo "$RENAME_NEW_DIVERGED"
    echo '```'
    echo
  fi

  if [ -n "$RENAME_ALREADY_DIVERGED" ]; then
    echo "### Renames of an already-diverged upstream-owned path (not newly diverged)"
    echo '```'
    echo "$RENAME_ALREADY_DIVERGED"
    echo '```'
    echo
  fi

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
}

FAIL_REASONS=()
[ -n "$NEW_CONFLICTS" ] && FAIL_REASONS+=("introduces new fixed-upstream merge-conflict surface")
[ -n "$NEW_DIVERGED" ] && FAIL_REASONS+=("diverges a previously upstream-exact file")
[ -n "$RENAME_NEW_DIVERGED" ] && FAIL_REASONS+=("renames a previously upstream-exact file away from tracking upstream")

if [ "${#FAIL_REASONS[@]}" -gt 0 ]; then
  echo "FAIL: this PR:"
  for reason in "${FAIL_REASONS[@]}"; do
    echo "  - $reason"
  done
  exit 1
fi

echo "PASS"
exit 0
