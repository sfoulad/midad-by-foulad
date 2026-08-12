#!/usr/bin/env bash
# measure-conflicts.sh <target-ref> [upstream-ref]
#
# Prints the file paths that would conflict if <upstream-ref> (default
# origin/upstream, the maintained CrossPoint mirror -- see
# .github/workflows/update-from-crosspoint.yml) were merged into
# <target-ref>, one per line, sorted. Empty output means a clean merge.
#
# Uses a disposable `git worktree` rather than checking out the temporary
# branch in the caller's own working tree: a plain `git checkout` would
# refuse to touch tracked paths the caller has uncommitted changes to
# ("local changes would be overwritten by merge"), silently aborting the
# merge attempt before it ever reaches conflict detection and making this
# script under-report conflicts (found the hard way running this locally
# against a dirty tree -- exit code 2, not the conflict path, empty output).
# A worktree starts from a clean index regardless of the caller's own
# working-tree state, so this can't happen. Never modifies <target-ref>
# itself; the worktree and its branch are always removed on exit, even on
# failure.
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

cleanup() {
  git worktree remove --force "$WORKTREE_DIR" >/dev/null 2>&1 || true
  rm -rf "$WORKTREE_DIR" >/dev/null 2>&1 || true
  git branch -D "$TMP_BRANCH" >/dev/null 2>&1 || true
}
trap cleanup EXIT

git worktree add --quiet -B "$TMP_BRANCH" "$WORKTREE_DIR" "$TARGET_REF" >/dev/null

(
  cd "$WORKTREE_DIR"
  if git merge --no-ff --no-edit "$UPSTREAM_REF" >/dev/null 2>&1; then
    : # clean merge -- nothing to report
  else
    git diff --name-only --diff-filter=U | sort
    git merge --abort >/dev/null 2>&1 || true
  fi
)
