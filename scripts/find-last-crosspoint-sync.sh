#!/usr/bin/env bash
# find-last-crosspoint-sync.sh <develop-ref> <upstream-ref>
#
# Prints the CrossPoint SHA that <develop-ref>'s most recent real sync merge
# (see .github/workflows/update-from-crosspoint.yml) actually merged in --
# i.e. how far into crosspoint-reader/develop Midad's develop has already
# absorbed. Prints nothing (exit 0, no output) if no recognized sync merge
# exists in <develop-ref>'s history yet -- meaning any commit on
# <upstream-ref> counts as "CrossPoint has advanced."
#
# A merge commit qualifies only if: it is a real merge (>=2 parents), one
# parent is an ancestor of <upstream-ref>, and its subject matches
# update-from-crosspoint.yml's exact generated pattern
# ("Merge crosspoint-reader/<ref> @ <sha>") -- same recognition test as
# scripts/thin-fork-guard.sh's is_recognized_sync_pr, applied to the whole
# history of <develop-ref> (not one PR's base..head slice), walking newest
# merge-commit first so the FIRST qualifying one found is the most recent.
#
# Fails closed: a `git log` failure (e.g. <develop-ref> not resolvable)
# aborts with a non-zero exit rather than silently printing nothing (which
# a caller could otherwise misread as "no prior sync, anything counts as
# advanced" when the real problem is that history couldn't be read at all).
set -euo pipefail

DEVELOP_REF="${1:?usage: find-last-crosspoint-sync.sh <develop-ref> <upstream-ref>}"
UPSTREAM_REF="${2:?usage: find-last-crosspoint-sync.sh <develop-ref> <upstream-ref>}"

if ! MERGE_LOG="$(git log --merges --format='%H %P' "$DEVELOP_REF" 2>&1)"; then
  echo "find-last-crosspoint-sync.sh: 'git log --merges $DEVELOP_REF' failed -- failing closed: $MERGE_LOG" >&2
  exit 1
fi

while IFS= read -r line; do
  [ -z "$line" ] && continue
  commit="${line%% *}"
  parents="${line#* }"
  subject="$(git log -1 --format=%s "$commit")"
  case "$subject" in
    "Merge crosspoint-reader/"*) : ;;
    *) continue ;;
  esac
  for p in $parents; do
    if git merge-base --is-ancestor "$p" "$UPSTREAM_REF" 2>/dev/null; then
      echo "$p"
      exit 0
    fi
  done
done <<<"$MERGE_LOG"

exit 0
