#!/usr/bin/env bash
# test-thin-fork-guard.sh
#
# Self-contained tests for thin-fork-guard.sh / measure-conflicts.sh.
# Builds tiny synthetic git repos per scenario (not real CrossPoint/Midad
# content -- this tests the algorithm, not today's actual conflict list)
# and asserts exit codes + key report lines. No network access, no
# dependency on this repo's own history.
#
# Scenarios 1-7 cover conflict-count gating and sync-PR recognition.
# Scenarios 8-12 cover the upstream-content divergence gate, the
# unconditional (non-bypassable) nature of both gates under sync-PR
# recognition, and measure-conflicts.sh's fail-closed behavior on a
# non-conflict merge failure. Scenario 13 covers renaming a previously
# upstream-exact file away from its upstream path, which the plain
# divergence gate cannot see on its own (see thin-fork-guard.sh's header).
#
# Usage: scripts/test-thin-fork-guard.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUARD="$SCRIPT_DIR/thin-fork-guard.sh"
MEASURE="$SCRIPT_DIR/measure-conflicts.sh"
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

# Variant of new_fixture adding shared3.txt, which NO branch ever touches
# at fixture-build time -- it is byte-identical across main/upstream_mirror
# /develop from the start, i.e. genuinely upstream-exact at base. shared.txt
# keeps the same pre-existing base conflict as new_fixture. Used by
# scenarios that need a clean (not-already-diverged) upstream-owned file to
# test the divergence gate in isolation from new_fixture's shared2.txt
# (which is already diverged at base by construction, so it can only ever
# land in "already diverged," never "newly diverged").
new_fixture_ext() {
  local dir="$WORKDIR/$1"
  mkdir -p "$dir"
  (
    cd "$dir" || exit 1
    git init -q -b main
    git config user.email "test@example.com"
    git config user.name "Test"

    echo "line1" >shared.txt
    echo "line1" >shared3.txt
    git add -A
    git commit -q -m "initial"

    git branch -q upstream_mirror
    git checkout -q upstream_mirror
    echo "line1-upstream-change" >shared.txt
    git commit -aqm "upstream changes shared.txt only"

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

# Fixture where `develop` has ALREADY cleanly merged upstream_mirror (a
# real --no-ff merge, so upstream_mirror is a git ancestor of develop) --
# simulating the world after the first full CrossPoint merge, where a
# previously-diverging file has since become byte-identical to upstream
# and the merge-conflict count for it reads 0 on both sides regardless of
# what a later PR does to it. Used to isolate the divergence gate from the
# conflict gate: a PR built on this base can newly diverge shared.txt
# without tripping any merge conflict at all (upstream_mirror has no
# commits beyond what's already merged, so re-merging it is a no-op).
new_fixture_synced_base() {
  local dir="$WORKDIR/$1"
  mkdir -p "$dir"
  (
    cd "$dir" || exit 1
    git init -q -b main
    git config user.email "test@example.com"
    git config user.name "Test"

    echo "line1" >shared.txt
    git add -A
    git commit -q -m "initial"

    git branch -q upstream_mirror
    git checkout -q upstream_mirror
    echo "line1-upstream-change" >shared.txt
    git commit -aqm "upstream changes shared.txt"

    git checkout -q main
    git branch -qf develop main
    git checkout -q develop
    git merge -q --no-ff upstream_mirror -m "Merge crosspoint-reader/develop @ deadbeef0001"
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

echo "=== Scenario 7: genuine clean sync, no additional divergence -> PASS ==="
# Post-sync tweak here only touches midad_only.txt (not upstream-owned), so
# no new conflict or divergence is introduced -- this must still PASS under
# the unconditional gates (contrast with scenario 9, which is identical up
# through the sync merge but then diverges an upstream-owned file and must
# FAIL even though it's an equally genuine, equally recognized sync PR).
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

echo "=== Scenario 8: post-ancestor edit of a previously upstream-exact file -> FAIL (divergence, zero new conflicts) ==="
{
  dir="$(new_fixture_synced_base scenario8)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  echo "line1-midad-diverges-post-sync" >shared.txt
  git commit -aqm "midad: diverge shared.txt after upstream is already an ancestor"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "post-ancestor divergence fails" 1 "$ec"
  assert_contains "post-ancestor divergence reports zero new conflicts" "No new conflicting files." "$out"
  assert_contains "post-ancestor divergence names shared.txt as newly diverged" "Files newly diverged from upstream content" "$out"
  assert_contains "post-ancestor divergence reports FAIL with divergence reason" "diverges a previously upstream-exact file" "$out"
}

echo "=== Scenario 9: genuine sync PR with a post-sync new divergence -> FAIL (sync recognition does not bypass) ==="
{
  dir="$(new_fixture_ext scenario9)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb "sync/crosspoint-20260102-000000"
  upstream_sha="$(git rev-parse upstream_mirror)"
  git merge --no-ff upstream_mirror -m "Merge crosspoint-reader/develop @ $upstream_sha" >/dev/null 2>&1 || true
  # Real conflict expected on shared.txt (diverged both sides) -- resolve
  # by taking upstream's content, exactly as a human would for a real sync.
  git checkout --theirs shared.txt 2>/dev/null || true
  git add -A
  git commit -qm "Merge crosspoint-reader/develop @ $upstream_sha" --no-edit >/dev/null 2>&1 || true
  # Post-sync Midad edit that diverges shared3.txt, which WAS upstream-exact
  # at base -- a valid sync's ancestry must not give this a free pass.
  echo "line1-midad-diverges-post-sync" >shared3.txt
  git commit -aqm "midad: post-sync divergence of shared3.txt"
  out="$("$GUARD" develop "sync/crosspoint-20260102-000000" upstream_mirror "sync/crosspoint-20260102-000000" 2>&1)"
  ec=$?
  assert_exit "genuine sync with post-sync divergence still fails" 1 "$ec"
  assert_contains "genuine sync with post-sync divergence still recognized as sync" "Recognized genuine CrossPoint sync PR" "$out"
  assert_contains "genuine sync with post-sync divergence names shared3.txt" "shared3.txt" "$out"
  assert_contains "genuine sync with post-sync divergence reports FAIL" "FAIL:" "$out"
}

echo "=== Scenario 10: deletion of a previously upstream-exact file -> FAIL (divergence) ==="
{
  dir="$(new_fixture_ext scenario10)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  git rm -q shared3.txt
  git commit -qm "midad: delete shared3.txt (was upstream-exact)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "deletion of upstream-exact file fails" 1 "$ec"
  assert_contains "deletion of upstream-exact file names shared3.txt as newly diverged" "shared3.txt" "$out"
  assert_contains "deletion of upstream-exact file reports FAIL with divergence reason" "diverges a previously upstream-exact file" "$out"
  assert_contains "deletion of upstream-exact file also flagged as an upstream-owned deletion" "Upstream-owned files deleted by this PR" "$out"
}

echo "=== Scenario 11: file returned to upstream-exact content -> PASS + resolved-divergence report ==="
{
  dir="$(new_fixture_ext scenario11)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  git show upstream_mirror:shared.txt >shared.txt
  git commit -aqm "midad: resolve shared.txt back to upstream-exact content"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "file returned to upstream-exact content passes" 0 "$ec"
  assert_contains "file returned to upstream-exact content reports resolved divergence" "Files returned to upstream-exact content" "$out"
  assert_contains "file returned to upstream-exact content reports PASS" "PASS" "$out"
}

echo "=== Scenario 12: measure-conflicts.sh fails closed on a non-conflict merge failure ==="
{
  dir="$(new_fixture_ext scenario12)"
  cd "$dir" || exit 1
  out="$("$MEASURE" develop nonexistent-ref-does-not-exist 2>&1)"
  ec=$?
  assert_exit "non-conflict merge failure exits nonzero" 1 "$ec"
  assert_contains "non-conflict merge failure reports the real reason on stderr" "failed for a" "$out"

  out2="$("$GUARD" develop develop nonexistent-ref-does-not-exist 2>&1)"
  ec2=$?
  assert_exit "guard propagates non-conflict merge failure as nonzero (fail-closed, not silent 0 conflicts)" 1 "$ec2"
  assert_contains "guard's propagated failure carries the real reason, not a swallowed PASS" "failed for a" "$out2"
}

echo "=== Scenario 13: rename of a previously upstream-exact file -> FAIL (divergence via rename, zero new conflicts) ==="
{
  dir="$(new_fixture_ext scenario13)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  git mv shared3.txt midad_shared3.txt
  git commit -qm "midad: rename shared3.txt away from its upstream-exact path"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "rename of upstream-exact file fails" 1 "$ec"
  assert_contains "rename of upstream-exact file reports zero new conflicts" "No new conflicting files." "$out"
  assert_contains "rename of upstream-exact file reports the old -> new mapping" "shared3.txt\` -> \`midad_shared3.txt" "$out"
  assert_contains "rename of upstream-exact file reports FAIL with the rename reason" "renames a previously upstream-exact file away from tracking upstream" "$out"
}

echo
echo "=== $TESTS_RUN assertions, $FAILURES failed ==="
if [ "$FAILURES" -ne 0 ]; then
  exit 1
fi
exit 0
