#!/usr/bin/env bash
# test-find-last-crosspoint-sync.sh
#
# Self-contained tests for find-last-crosspoint-sync.sh. Builds tiny
# synthetic git repos with a real "upstream" remote-tracking branch and a
# real `--no-ff` merge whose message matches update-from-crosspoint.yml's
# exact generated pattern, so the recognition logic is exercised for real,
# not mocked.
#
# Scenario 1: no sync merge has ever happened -> prints nothing, exit 0.
# Scenario 2: one real sync merge exists -> prints its upstream-tracking
#             parent SHA.
# Scenario 3: two sync merges exist -> prints the NEWEST one's upstream
#             parent, not the oldest.
# Scenario 4: an ordinary (non-sync) merge commit exists alongside a real
#             sync merge -> the ordinary merge is ignored, the sync merge
#             is still found.
# Scenario 5: unresolvable develop-ref -> fails closed (non-zero exit).
#
# Usage: scripts/test-find-last-crosspoint-sync.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FINDER="$SCRIPT_DIR/find-last-crosspoint-sync.sh"
WORKDIR="$(mktemp -d)"
FAILURES=0
TESTS_RUN=0

# shellcheck disable=SC2329 # invoked indirectly via `trap ... EXIT` below
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

pass() { echo "  ok: $1"; }
fail() { echo "  FAIL: $1"; FAILURES=$((FAILURES + 1)); }

assert_eq() {
  local desc="$1" expected="$2" actual="$3"
  TESTS_RUN=$((TESTS_RUN + 1))
  if [ "$actual" = "$expected" ]; then
    pass "$desc"
  else
    fail "$desc (expected '$expected', got '$actual')"
  fi
}

assert_exit() {
  local desc="$1" expected="$2" actual="$3"
  TESTS_RUN=$((TESTS_RUN + 1))
  if [ "$actual" = "$expected" ]; then
    pass "$desc (exit $actual)"
  else
    fail "$desc (expected exit $expected, got $actual)"
  fi
}

# new_pair <dir> builds two repos: <dir>/upstream (plays the role of
# crosspoint-reader/develop) and <dir>/midad (plays the role of Midad's
# develop, with `origin/upstream` -- actually a real remote named
# "upstream" pointing at <dir>/upstream -- fetched into it).
new_pair() {
  local dir="$1"
  mkdir -p "$dir/upstream" "$dir/midad"

  git -C "$dir/upstream" init -q
  git -C "$dir/upstream" checkout -q -b master
  git -C "$dir/upstream" config user.name test
  git -C "$dir/upstream" config user.email test@example.com
  echo "up1" >"$dir/upstream/file.txt"
  git -C "$dir/upstream" add -A
  git -C "$dir/upstream" commit -q -m "upstream commit 1"

  git -C "$dir/midad" init -q
  git -C "$dir/midad" checkout -q -b master
  git -C "$dir/midad" config user.name test
  git -C "$dir/midad" config user.email test@example.com
  echo "midad1" >"$dir/midad/README.md"
  git -C "$dir/midad" add -A
  git -C "$dir/midad" commit -q -m "midad base"
  git -C "$dir/midad" remote add upstream "$dir/upstream"
  git -C "$dir/midad" fetch -q upstream
}

# real_sync_merge <dir> <upstream-sha-var-name> performs an actual
# `--no-ff` merge of upstream/master into midad's current branch, with a
# commit message matching the exact generated pattern.
real_sync_merge() {
  local dir="$1"
  local upstream_head
  upstream_head="$(git -C "$dir/upstream" rev-parse HEAD)"
  git -C "$dir/midad" fetch -q upstream
  git -C "$dir/midad" merge --no-ff -q --allow-unrelated-histories "upstream/master" \
    -m "Merge crosspoint-reader/develop @ $upstream_head"
  echo "$upstream_head"
}

echo "=== find-last-crosspoint-sync.sh test suite ==="
echo

echo "--- Scenario 1: no sync merge yet -> nothing printed ---"
dir="$WORKDIR/s1"
new_pair "$dir"
output="$("$FINDER" "$dir/midad" "$dir" 2>&1)"
# (upstream-ref arg is irrelevant here since there's no merge to test
# ancestry against; pass the midad dir itself as a harmless placeholder.)
output="$(cd "$dir/midad" && "$FINDER" HEAD upstream/master 2>&1)"
assert_eq "no output when no sync has happened" "" "$output"
echo

echo "--- Scenario 2: one real sync merge -> its upstream parent printed ---"
dir="$WORKDIR/s2"
new_pair "$dir"
sha1="$(real_sync_merge "$dir")"
output="$(cd "$dir/midad" && "$FINDER" HEAD upstream/master 2>&1)"
assert_eq "prints the sync's upstream parent" "$sha1" "$output"
echo

echo "--- Scenario 3: two sync merges -> newest one's parent printed ---"
dir="$WORKDIR/s3"
new_pair "$dir"
real_sync_merge "$dir" >/dev/null
echo "up2" >"$dir/upstream/file.txt"
git -C "$dir/upstream" add -A
git -C "$dir/upstream" commit -q -m "upstream commit 2"
sha2="$(real_sync_merge "$dir")"
output="$(cd "$dir/midad" && "$FINDER" HEAD upstream/master 2>&1)"
assert_eq "prints the NEWEST sync's upstream parent" "$sha2" "$output"
echo

echo "--- Scenario 4: ordinary merge alongside a real sync merge ---"
dir="$WORKDIR/s4"
new_pair "$dir"
sha1="$(real_sync_merge "$dir")"
git -C "$dir/midad" checkout -q -b feature
echo "feat" >"$dir/midad/feature.txt"
git -C "$dir/midad" add -A
git -C "$dir/midad" commit -q -m "feature work"
git -C "$dir/midad" checkout -q master 2>/dev/null || git -C "$dir/midad" checkout -q main
git -C "$dir/midad" merge --no-ff -q feature -m "Merge feature branch"
output="$(cd "$dir/midad" && "$FINDER" HEAD upstream/master 2>&1)"
assert_eq "ordinary merge ignored, real sync still found" "$sha1" "$output"
echo

echo "--- Scenario 5: unresolvable develop-ref -> fails closed ---"
dir="$WORKDIR/s5"
new_pair "$dir"
set +e
output="$(cd "$dir/midad" && "$FINDER" does-not-exist upstream/master 2>&1)"
exit_code=$?
set -e
assert_exit "unresolvable ref fails closed" "1" "$exit_code"
echo

echo "=== Results: $((TESTS_RUN - FAILURES))/$TESTS_RUN passed ==="
if [ "$FAILURES" -gt 0 ]; then
  exit 1
fi
exit 0
