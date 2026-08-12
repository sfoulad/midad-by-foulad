#!/usr/bin/env bash
# measure-conflicts.sh <target-ref> [upstream-ref]
#
# Prints the file paths that would conflict if <upstream-ref> (default
# origin/upstream, the maintained CrossPoint mirror -- see
# .github/workflows/update-from-crosspoint.yml) were merged into
# <target-ref>, one per line, sorted. Empty output means a clean merge.
#
# Fails closed: a merge can fail for reasons that are NOT a content
# conflict (bad ref, detached-HEAD/identity issues, an underlying git
# error) -- treating every non-zero merge exit as "conflicts, read them
# from --diff-filter=U" silently produces an empty list for those cases,
# which looks identical to "clean merge" to a caller. Distinguished here:
#   - merge exit 0                              -> clean, empty output, exit 0
#   - merge non-zero + unmerged (U) paths exist  -> genuine conflict, print
#                                                    them, exit 0
#   - merge non-zero + no unmerged paths         -> ERROR: print the raw
#                                                    git failure to stderr,
#                                                    exit 1 (the caller,
#                                                    thin-fork-guard.sh, is
#                                                    expected to propagate
#                                                    this and fail CI, not
#                                                    treat it as "no
#                                                    conflicts")
#
# Uses a disposable `git worktree` rather than checking out the temporary
# branch in the caller's own working tree: a plain `git checkout` would
# refuse to touch tracked paths the caller has uncommitted changes to
# ("local changes would be overwritten by merge"), silently aborting the
# merge attempt before it ever reaches conflict detection and making this
# script under-report conflicts (found the hard way running this locally
# against a dirty tree -- exit code 2, not the conflict path, empty
# output; this is exactly the fail-open bug this script's error handling
# above now catches on purpose instead of silently swallowing). A worktree
# starts from a clean index regardless of the caller's own working-tree
# state, so this can't happen. Never modifies <target-ref> itself; the
# worktree and its branch are always removed on exit, even on failure.
#
# This automates the real-merge methodology
# docs/upstream-sync-architecture.md's "Phase A results" section
# establishes as the authoritative conflict count (a genuine
# `git merge --no-ff`, not a --numstat divergence heuristic) -- see that
# doc's "Running the audit yourself" section for the manual equivalent
# this replaces for CI purposes. Manual one-off checks against a FIXED
# historical baseline commit (as used during BLE-R1/R2 review) remain
# useful for tracking progress over time, but a CI gate needs the live
# mirror at run time -- see thin-fork-guard.sh's own header for why.
set -euo pipefail

TARGET_REF="${1:?usage: measure-conflicts.sh <target-ref> [upstream-ref]}"
UPSTREAM_REF="${2:-origin/upstream}"

WORKTREE_DIR="$(mktemp -d)"
TMP_BRANCH="tmp-conflict-check-$$-$RANDOM"
MERGE_LOG="$(mktemp)"

# shellcheck disable=SC2329 # invoked indirectly via `trap ... EXIT` below
cleanup() {
  git -C "$WORKTREE_DIR" merge --abort >/dev/null 2>&1 || true
  git worktree remove --force "$WORKTREE_DIR" >/dev/null 2>&1 || true
  rm -rf "$WORKTREE_DIR" "$MERGE_LOG" >/dev/null 2>&1 || true
  git branch -D "$TMP_BRANCH" >/dev/null 2>&1 || true
}
trap cleanup EXIT

git worktree add --quiet -B "$TMP_BRANCH" "$WORKTREE_DIR" "$TARGET_REF" >/dev/null

set +e
git -C "$WORKTREE_DIR" merge --no-ff --no-edit "$UPSTREAM_REF" >"$MERGE_LOG" 2>&1
merge_status=$?
set -e

if [ "$merge_status" -eq 0 ]; then
  exit 0 # clean merge -- nothing to report
fi

unmerged="$(git -C "$WORKTREE_DIR" diff --name-only --diff-filter=U)"
if [ -n "$unmerged" ]; then
  echo "$unmerged" | sort
  exit 0
fi

{
  echo "measure-conflicts.sh: merge of '$UPSTREAM_REF' into '$TARGET_REF' failed for a"
  echo "reason other than a content conflict (git exited $merge_status with no unmerged"
  echo "paths). Raw git output:"
  echo
  cat "$MERGE_LOG"
} >&2
exit 1
