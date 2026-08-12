#!/usr/bin/env bash
# thin-fork-guard.sh <base-ref> <head-ref> [upstream-ref] [head-branch-name]
#
# Reports, and gates on, whether <head-ref> introduces new fixed-upstream
# merge-conflict surface relative to <base-ref>, by simulating a real merge
# of <upstream-ref> (default origin/upstream, the maintained CrossPoint
# mirror kept current by .github/workflows/update-from-crosspoint.yml) into
# each side. See CLAUDE.md's "Midad Thin-Fork Architecture" section and
# docs/upstream-sync-architecture.md for why this exists.
#
# Deliberately compares against the LIVE upstream mirror at run time, not a
# hard-coded historical commit: base and head are measured against the
# identical mirror snapshot within one run, so the only variable is the
# PR's own diff -- that stays correct forever, including after the first
# full CrossPoint merge, without ever needing today's number hard-coded.
#
# Exit 0: no new conflicting files, OR a recognized genuine CrossPoint sync
#         PR (see is_recognized_sync_pr below -- report-only in that case).
# Exit 1: otherwise.
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
# renamed to that prefix must not bypass the guard. Recognition requires a
# real merge commit unique to <head-ref> whose parent is (an ancestor of)
# <upstream-ref>'s current tip AND whose message matches
# update-from-crosspoint.yml's exact generated pattern
# ("Merge crosspoint-reader/<ref> @ <sha>"). Run unconditionally, regardless
# of branch name -- name is only used below for a mismatch warning.
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
trap 'rm -f "$BASE_CONFLICTS_FILE" "$HEAD_CONFLICTS_FILE"' EXIT

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
# tracking these going forward. Not silently guessed either way -- flagged
# for human review. See CLAUDE.md/docs/upstream-sync-architecture.md.
RENAME_WARNINGS=""
while IFS=$'\t' read -r _ old_path new_path; do
  [ -z "$old_path" ] && continue
  if git cat-file -e "$UPSTREAM_REF:$old_path" 2>/dev/null; then
    RENAME_WARNINGS="${RENAME_WARNINGS}\`${old_path}\` -> \`${new_path}\`"$'\n'
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

# --- 3. Sync-PR recognition --------------------------------------------------
IS_SYNC=false
NAME_MISMATCH=false
if is_recognized_sync_pr "$BASE_REF" "$HEAD_REF" "$UPSTREAM_REF"; then
  IS_SYNC=true
elif [[ "$HEAD_BRANCH_NAME" == sync/crosspoint-* ]]; then
  NAME_MISMATCH=true
fi

# --- 4. Report ---------------------------------------------------------------
{
  echo "## Thin-Fork Guard"
  echo
  echo "Base (\`$BASE_REF\`) conflicting files: **$BASE_COUNT**"
  echo "PR HEAD (\`$HEAD_REF\`) conflicting files: **$HEAD_COUNT**"
  echo

  if [ "$IS_SYNC" = true ]; then
    echo "**Recognized genuine CrossPoint sync PR** (verified upstream-merge ancestry from a real \`update-from-crosspoint.yml\` run). Running in report-only mode -- conflict-count fluctuation during a real sync is expected, per docs/upstream-sync-architecture.md's human-review step."
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

  echo "### Upstream-owned files touched by this PR"
  if [ -z "$UPSTREAM_TOUCHED" ]; then
    echo "None."
  else
    echo "| File | +/- | Already conflicting (base) |"
    echo "|---|---|---|"
    while IFS= read -r path; do
      [ -z "$path" ] && continue
      stats="$(git diff --numstat "$BASE_REF...$HEAD_REF" -- "$path")"
      add="$(echo "$stats" | awk '{print $1}')"
      del="$(echo "$stats" | awk '{print $2}')"
      if grep -Fxq "$path" "$BASE_CONFLICTS_FILE"; then
        already="yes"
      else
        already="no"
      fi
      echo "| \`$path\` | +${add:-0}/-${del:-0} | $already |"
    done <<<"$UPSTREAM_TOUCHED"
  fi
  echo

  if [ -n "$RENAME_WARNINGS" ]; then
    echo "### Renames away from an upstream-owned path (needs human review)"
    echo "$RENAME_WARNINGS"
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

if [ -n "$NEW_CONFLICTS" ] && [ "$IS_SYNC" = false ]; then
  echo "FAIL: this PR introduces new fixed-upstream conflict surface."
  exit 1
fi

echo "PASS"
exit 0
