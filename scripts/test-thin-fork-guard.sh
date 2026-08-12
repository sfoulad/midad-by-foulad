#!/usr/bin/env bash
# test-thin-fork-guard.sh
#
# Self-contained tests for thin-fork-guard.sh / measure-conflicts.sh.
# Builds tiny synthetic git repos per scenario (not real CrossPoint/Midad
# content -- this tests the algorithm, not today's actual conflict list)
# and asserts exit codes + key report lines. No network access, no
# dependency on this repo's own history.
#
# Usage: scripts/test-thin-fork-guard.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUARD="$SCRIPT_DIR/thin-fork-guard.sh"
WORKDIR="$(mktemp -d)"
FAILURES=0
TESTS_RUN=0

# shellcheck disable=SC2329 # invoked indirectly via `trap ... EXIT` below
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

pass() { echo "  ok: $1"; }
fail() { echo "  FAIL: $1"; FAILURES=$((FAILURES + 1)); }

assert_exit() {
  local desc="$1" expected="$2" actual="$3"
  TESTS_RUN=$((TESTS_RUN + 1))
  if [ "$actual" = "$expected" ]; then
    pass "$desc (exit $actual)"
  else
    fail "$desc (expected exit $expected, got $actual)"
  fi
}

assert_contains() {
  local desc="$1" needle="$2" haystack="$3"
  TESTS_RUN=$((TESTS_RUN + 1))
  if grep -qF "$needle" <<<"$haystack"; then
    pass "$desc"
  else
    fail "$desc (output did not contain: $needle)"
  fi
}

# Builds a fresh fixture repo at $WORKDIR/<name> with:
#   main              -- shared ancestor, two "upstream-owned" files
#   upstream_mirror    -- diverges main by changing both shared files
#   develop            -- diverges main by changing shared.txt differently
#                          (the pre-existing base conflict) and adding a
#                          Midad-only file. shared2.txt is untouched here,
#                          so it does NOT conflict yet.
new_fixture() {
  local dir="$WORKDIR/$1"
  mkdir -p "$dir"
  (
    cd "$dir" || exit 1
    git init -q -b main
    git config user.email "test@example.com"
    git config user.name "Test"

    echo "line1" >shared.txt
    echo "line1" >shared2.txt
    git add -A
    git commit -q -m "initial"

    git branch -q upstream_mirror
    git checkout -q upstream_mirror
    echo "line1-upstream-change" >shared.txt
    echo "line1-upstream-change" >shared2.txt
    git commit -aqm "upstream changes shared files"

    git checkout -q main
    git branch -qf develop main
    git checkout -q develop
    echo "line1-midad-change" >shared.txt
    echo "midad content" >midad_only.txt
    git add -A
    git commit -qm "midad: diverge shared.txt + add midad-only file"
  )
  echo "$dir"
}

echo "=== Scenario 1: no conflict change -> PASS ==="
{
  dir="$(new_fixture scenario1)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  echo "more midad content" >>midad_only.txt
  git commit -aqm "midad-only change"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "no conflict change" 0 "$ec"
  assert_contains "no conflict change reports PASS" "PASS" "$out"
  assert_contains "no conflict change reports no new conflicts" "No new conflicting files." "$out"
}

echo "=== Scenario 2: conflict removed -> PASS ==="
{
  dir="$(new_fixture scenario2)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  git show upstream_mirror:shared.txt >shared.txt
  git commit -aqm "resolve shared.txt to match upstream"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "conflict removed" 0 "$ec"
  assert_contains "conflict removed reports resolved section" "Conflicts resolved by this PR" "$out"
}

echo "=== Scenario 3: one new conflict -> FAIL ==="
{
  dir="$(new_fixture scenario3)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  echo "line1-midad-change-2" >shared2.txt
  git commit -aqm "midad: diverge shared2.txt too"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "one new conflict" 1 "$ec"
  assert_contains "new conflict names shared2.txt" "shared2.txt" "$out"
  assert_contains "new conflict reports FAIL" "FAIL:" "$out"
}

echo "=== Scenario 4: modify already-conflicting upstream file -> PASS + report ==="
{
  dir="$(new_fixture scenario4)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  echo "line1-midad-change, more midad-specific content" >shared.txt
  git commit -aqm "midad: add more content to already-conflicting shared.txt"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "modify already-conflicting file" 0 "$ec"
  assert_contains "already-conflicting file reported as yes" "shared.txt\` | +1/-1 | yes |" "$out"
}

echo "=== Scenario 5: Midad-only new file -> PASS ==="
{
  dir="$(new_fixture scenario5)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  echo "brand new midad file" >new_midad_file.txt
  git add new_midad_file.txt
  git commit -qm "midad: add brand-new file, not upstream-owned"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "midad-only new file" 0 "$ec"
}

echo "=== Scenario 6: fake sync/crosspoint-* branch, no real ancestry -> must NOT bypass ==="
{
  dir="$(new_fixture scenario6)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb "sync/crosspoint-fake123"
  echo "line1-midad-change-2" >shared2.txt
  git commit -aqm "midad: diverge shared2.txt too (fake sync branch)"
  out="$("$GUARD" develop "sync/crosspoint-fake123" upstream_mirror "sync/crosspoint-fake123" 2>&1)"
  ec=$?
  assert_exit "fake sync branch does not bypass" 1 "$ec"
  assert_contains "fake sync branch flags name mismatch" "does NOT contain a verified upstream-merge ancestry" "$out"
  assert_contains "fake sync branch still fails on new conflict" "FAIL:" "$out"
}

echo "=== Scenario 7: genuine upstream sync ancestry -> recognized correctly ==="
{
  dir="$(new_fixture scenario7)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb "sync/crosspoint-20260101-000000"
  upstream_sha="$(git rev-parse upstream_mirror)"
  git merge --no-ff upstream_mirror -m "Merge crosspoint-reader/develop @ $upstream_sha" >/dev/null 2>&1 || true
  # Real conflict expected here (shared.txt diverged both sides) -- resolve
  # by taking upstream's content, exactly as a human would for a real sync.
  git checkout --theirs shared.txt 2>/dev/null || true
  git add -A
  git commit -qm "Merge crosspoint-reader/develop @ $upstream_sha" --no-edit >/dev/null 2>&1 || true
  # A small follow-up Midad tweak after the sync merge, same as a real sync
  # PR might carry (e.g. a hook adjustment).
  echo "post-sync midad tweak" >>midad_only.txt
  git commit -aqm "midad: post-sync tweak"
  out="$("$GUARD" develop "sync/crosspoint-20260101-000000" upstream_mirror "sync/crosspoint-20260101-000000" 2>&1)"
  ec=$?
  assert_exit "genuine sync PR recognized" 0 "$ec"
  assert_contains "genuine sync PR reports recognition" "Recognized genuine CrossPoint sync PR" "$out"
}

echo
echo "=== $TESTS_RUN assertions, $FAILURES failed ==="
if [ "$FAILURES" -ne 0 ]; then
  exit 1
fi
exit 0
