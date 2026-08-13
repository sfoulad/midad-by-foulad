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
# Scenarios 14-15 cover the merge-base-anchored Midad-side divergence gate
# specifically: a file upstream has already moved since the last shared
# history, which a naive "differs from upstream's current tip" comparison
# misclassifies as "already diverged" even when Midad has never touched it
# -- see thin-fork-guard.sh's "COMMON_REF" header comment for the four-state
# model this exists to get right. Scenarios 16-18 cover the two independent
# security-boundary gates (SECURITY_BOUNDARY_FILES and the workflow-
# permission audit) that protect the guard's own enforcement machinery --
# unrelated to thin-fork divergence, so they use the plain new_fixture
# fixture with no upstream-ownership significance to the paths involved.
# Scenarios 19-20 cover renaming a security-boundary file AWAY (the same
# "--name-only doesn't surface a rename's old path" bypass class as
# scenario 13, but for the security-boundary gate specifically, which is
# independent code from the thin-fork rename gate). Scenario 21-23 and 25
# prove the workflow-permission audit is a real YAML parse, not a text
# match against one exact spelling: quoting, spacing, and a YAML alias all
# still get caught because the parser reads the resolved value, not the
# source bytes. Scenario 24 covers renaming an unrelated file INTO a
# security-boundary pathname. Scenario 26 covers deleting a security-
# boundary file outright. Scenario 27 proves the workflow-permission audit
# fails closed (hard aborts) on a workflow file it cannot parse, rather
# than silently treating an unreadable file as "contains no violations."
# Scenario 28 covers scripts/test-thin-fork-guard.sh itself as protected
# security-boundary code -- the trusted workflow executes it before the
# real guard, so a modified copy is trusted executable code, not just a
# governance-flagged file. Scenarios 29-32 cover the "every
# pull_request_target workflow must declare explicit top-level
# permissions" invariant: no declaration at all, explicit read-only
# permissions (PASS), explicit permissions that still contain a forbidden
# write (routes through the existing forbidden-permission rule, not the
# missing-declaration one), and the mapping-form/array-form trigger
# spellings (both must be recognized, not just the bare scalar form).
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

# Variant of new_fixture adding shared3.txt, shared4.txt, and shared5.txt,
# none of which `develop` ever touches at fixture-build time -- they start
# Midad-clean (base == the shared merge-base with upstream). shared.txt
# keeps the same pre-existing base conflict as new_fixture. Used by
# scenarios that need a clean (not-already-diverged) shared file to test
# the divergence gate in isolation from new_fixture's shared2.txt (which is
# already diverged at base by construction, so it can only ever land in
# "already diverged," never "newly diverged").
#
# shared3.txt: single-line, untouched by upstream_mirror too -- a plain
#   Midad-clean file for deletion/rename/resolve-to-exact scenarios.
# shared4.txt: multi-line; upstream_mirror changes line A only. Lets a
#   scenario change a DIFFERENT line (line C) on the Midad side and still
#   get a clean 3-way merge (disjoint line changes), proving the
#   Midad-side divergence gate is load-bearing independent of the conflict
#   gate even when upstream has moved since the shared merge-base.
# shared5.txt: upstream_mirror DELETES it entirely (Midad's copy survives
#   unchanged at base) -- covers "upstream deletes a path after the shared
#   merge-base while base still contains it."
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
    printf 'lineA\nlineB\nlineC\n' >shared4.txt
    echo "keep me" >shared5.txt
    git add -A
    git commit -q -m "initial"

    git branch -q upstream_mirror
    git checkout -q upstream_mirror
    echo "line1-upstream-change" >shared.txt
    printf 'lineA-upstream\nlineB\nlineC\n' >shared4.txt
    git rm -q shared5.txt
    git commit -aqm "upstream changes shared.txt/shared4.txt line A, deletes shared5.txt"

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
  assert_contains "post-ancestor divergence reports FAIL with divergence reason" "introduces new Midad-side divergence in a file that was clean relative to the shared upstream history" "$out"
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
  assert_contains "deletion of upstream-exact file reports FAIL with divergence reason" "introduces new Midad-side divergence in a file that was clean relative to the shared upstream history" "$out"
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

  # A bogus upstream ref is now caught even earlier than measure-conflicts.sh
  # -- thin-fork-guard.sh computes COMMON_REF via `git merge-base` as its
  # very first step, which fails for the same bad ref before ever reaching
  # measure-conflicts.sh. Still fail-closed, just via an earlier check; the
  # standalone $MEASURE assertions above already cover measure-conflicts.sh's
  # own fail-closed behavior in isolation.
  out2="$("$GUARD" develop develop nonexistent-ref-does-not-exist 2>&1)"
  ec2=$?
  assert_exit "guard propagates a bad upstream ref as nonzero (fail-closed, not silent 0 conflicts)" 1 "$ec2"
  assert_contains "guard's propagated failure carries the real reason, not a swallowed PASS" "cannot reason about upstream divergence" "$out2"
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
  assert_contains "rename of upstream-exact file reports FAIL with the rename reason" "renames a Midad-clean shared file away from tracking upstream" "$out"
}

echo "=== Scenario 14: upstream moves first, Midad then edits a different line -> FAIL (Midad-side divergence, zero new conflicts) ==="
{
  dir="$(new_fixture_ext scenario14)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  printf 'lineA\nlineB\nlineC-midad\n' >shared4.txt
  git commit -aqm "midad: edit shared4.txt line C (upstream already moved line A first)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "upstream-moved-first then Midad edit fails" 1 "$ec"
  assert_contains "upstream-moved-first then Midad edit reports zero new conflicts" "No new conflicting files." "$out"
  assert_contains "upstream-moved-first then Midad edit names shared4.txt" "shared4.txt" "$out"
  assert_contains "upstream-moved-first then Midad edit reports the Midad-side divergence section" "New Midad-side divergence" "$out"
  assert_contains "upstream-moved-first then Midad edit reports FAIL with the Midad-side reason" "introduces new Midad-side divergence in a file that was clean relative to the shared upstream history" "$out"
}

echo "=== Scenario 15: upstream deletes a path after the shared history, Midad then edits it -> FAIL ==="
{
  dir="$(new_fixture_ext scenario15)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  echo "midad edited" >shared5.txt
  git commit -aqm "midad: edit shared5.txt (upstream already deleted it)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  # Deleting a file on one side and modifying it on the other is an
  # inherent git modify/delete merge conflict -- this scenario cannot be
  # made conflict-clean the way scenario 14 is. The point here is proving
  # the divergence classification does NOT mislabel shared5.txt as
  # "already diverged" just because upstream's deletion makes its blob
  # differ from Midad's copy -- it must recognize this as Midad's FIRST
  # touch to a previously clean file.
  assert_exit "upstream-deleted-first then Midad edit fails" 1 "$ec"
  assert_contains "upstream-deleted-first then Midad edit names shared5.txt" "shared5.txt" "$out"
  assert_contains "upstream-deleted-first then Midad edit reports the Midad-side divergence section" "New Midad-side divergence" "$out"
}

echo "=== Scenario 16: ordinary PR modifies a security-boundary file -> FAIL ==="
{
  dir="$(new_fixture scenario16)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  echo "name: CI" >.github/workflows/ci.yml
  git add .github/workflows/ci.yml
  git commit -qm "ordinary: touch ci.yml (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "security-boundary file touch fails" 1 "$ec"
  assert_contains "security-boundary file touch names ci.yml" ".github/workflows/ci.yml" "$out"
  assert_contains "security-boundary file touch reports the gate section" "Security-boundary files touched by this PR" "$out"
  assert_contains "security-boundary file touch reports FAIL with the right reason" "security-boundary file modified -- explicit guard-maintenance procedure required" "$out"
}

echo "=== Scenario 17: ordinary PR adds a workflow with statuses: write -> FAIL ==="
{
  dir="$(new_fixture scenario17)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: Suspicious\non:\n  pull_request_target:\npermissions:\n  statuses: write\n' >.github/workflows/suspicious.yml
  git add .github/workflows/suspicious.yml
  git commit -qm "ordinary: add a workflow claiming statuses: write (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "new statuses:write workflow fails" 1 "$ec"
  assert_contains "new statuses:write workflow names suspicious.yml" "suspicious.yml" "$out"
  assert_contains "new statuses:write workflow reports the audit section" "Workflow files with forbidden status/check-write permissions" "$out"
  assert_contains "new statuses:write workflow reports FAIL with the right reason" "introduces a workflow with status/check-write permissions outside thin-fork-guard-trusted.yml" "$out"
}

echo "=== Scenario 18: ordinary PR adds a normal read-only workflow -> PASS ==="
{
  dir="$(new_fixture scenario18)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: Harmless\non:\n  pull_request:\npermissions:\n  contents: read\n' >.github/workflows/harmless.yml
  git add .github/workflows/harmless.yml
  git commit -qm "ordinary: add a harmless read-only workflow"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "harmless read-only workflow passes" 0 "$ec"
  assert_contains "harmless read-only workflow reports PASS" "PASS" "$out"
}

echo "=== Scenario 19: rename scripts/thin-fork-guard.sh away -> FAIL (security-boundary) ==="
{
  dir="$(new_fixture scenario19)"
  cd "$dir" || exit 1
  git checkout -q develop
  mkdir -p scripts
  echo "#!/usr/bin/env bash" >scripts/thin-fork-guard.sh
  git add scripts/thin-fork-guard.sh
  git commit -qm "develop: placeholder thin-fork-guard.sh"
  git checkout -qb head_branch
  git mv scripts/thin-fork-guard.sh scripts/old-thin-fork-guard.sh
  git commit -qm "ordinary: rename thin-fork-guard.sh away (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "rename thin-fork-guard.sh away fails" 1 "$ec"
  # shellcheck disable=SC2016 # single-quoted on purpose: backticks must stay literal, not become command substitution
  assert_contains "rename thin-fork-guard.sh away names the rename" 'scripts/thin-fork-guard.sh` -> `scripts/old-thin-fork-guard.sh' "$out"
  assert_contains "rename thin-fork-guard.sh away reports FAIL with the right reason" "security-boundary file modified -- explicit guard-maintenance procedure required" "$out"
}

echo "=== Scenario 20: rename .github/workflows/ci.yml away -> FAIL (security-boundary) ==="
{
  dir="$(new_fixture scenario20)"
  cd "$dir" || exit 1
  git checkout -q develop
  mkdir -p .github/workflows
  echo "name: CI" >.github/workflows/ci.yml
  git add .github/workflows/ci.yml
  git commit -qm "develop: placeholder ci.yml"
  git checkout -qb head_branch
  git mv .github/workflows/ci.yml .github/workflows/ci-disabled.yml
  git commit -qm "ordinary: rename ci.yml away (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "rename ci.yml away fails" 1 "$ec"
  # shellcheck disable=SC2016 # single-quoted on purpose: backticks must stay literal, not become command substitution
  assert_contains "rename ci.yml away names the rename" '.github/workflows/ci.yml` -> `.github/workflows/ci-disabled.yml' "$out"
  assert_contains "rename ci.yml away reports FAIL with the right reason" "security-boundary file modified -- explicit guard-maintenance procedure required" "$out"
}

echo '=== Scenario 21: workflow permission audit catches quoted statuses: "write" -> FAIL ==='
{
  dir="$(new_fixture scenario21)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: Suspicious\non:\n  pull_request_target:\npermissions:\n  statuses: "write"\n' >.github/workflows/suspicious21.yml
  git add .github/workflows/suspicious21.yml
  git commit -qm "ordinary: add workflow with quoted statuses write (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "quoted statuses write fails" 1 "$ec"
  assert_contains "quoted statuses write names the file" "suspicious21.yml" "$out"
  assert_contains "quoted statuses write reports FAIL with the right reason" "introduces a workflow with status/check-write permissions outside thin-fork-guard-trusted.yml" "$out"
}

echo '=== Scenario 22: workflow permission audit catches quoted permissions: "write-all" -> FAIL ==='
{
  dir="$(new_fixture scenario22)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: Suspicious\non:\n  pull_request_target:\npermissions: "write-all"\n' >.github/workflows/suspicious22.yml
  git add .github/workflows/suspicious22.yml
  git commit -qm "ordinary: add workflow with quoted write-all (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "quoted write-all fails" 1 "$ec"
  assert_contains "quoted write-all names the file" "suspicious22.yml" "$out"
  assert_contains "quoted write-all reports FAIL with the right reason" "introduces a workflow with status/check-write permissions outside thin-fork-guard-trusted.yml" "$out"
}

echo "=== Scenario 23: workflow permission audit catches a quoted/spaced checks: write variant -> FAIL ==="
{
  dir="$(new_fixture scenario23)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: Suspicious\non:\n  pull_request_target:\npermissions:\n  "checks"   :   write\n' >.github/workflows/suspicious23.yml
  git add .github/workflows/suspicious23.yml
  git commit -qm "ordinary: add workflow with quoted/spaced checks write (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "quoted/spaced checks write fails" 1 "$ec"
  assert_contains "quoted/spaced checks write names the file" "suspicious23.yml" "$out"
  assert_contains "quoted/spaced checks write reports FAIL with the right reason" "introduces a workflow with status/check-write permissions outside thin-fork-guard-trusted.yml" "$out"
}

echo "=== Scenario 24: rename an unrelated file INTO a security-boundary pathname -> FAIL ==="
{
  dir="$(new_fixture scenario24)"
  cd "$dir" || exit 1
  git checkout -q develop
  mkdir -p scripts
  echo "some other script content, long enough for git's rename similarity heuristic" >scripts/harmless-tool.sh
  git add scripts/harmless-tool.sh
  git commit -qm "develop: unrelated harmless script"
  git checkout -qb head_branch
  git mv scripts/harmless-tool.sh scripts/measure-conflicts.sh
  git commit -qm "ordinary: rename an unrelated file into a boundary pathname (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "rename into boundary pathname fails" 1 "$ec"
  # shellcheck disable=SC2016 # single-quoted on purpose: backticks must stay literal, not become command substitution
  assert_contains "rename into boundary pathname names the rename" 'scripts/harmless-tool.sh` -> `scripts/measure-conflicts.sh' "$out"
  assert_contains "rename into boundary pathname reports FAIL with the right reason" "security-boundary file modified -- explicit guard-maintenance procedure required" "$out"
}

echo "=== Scenario 25: workflow permission audit catches a YAML alias resolving to write -> FAIL (aliases cannot bypass) ==="
{
  dir="$(new_fixture scenario25)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'x-perm: &writeperm write\nname: Suspicious\non:\n  pull_request_target:\npermissions:\n  statuses: *writeperm\n' >.github/workflows/suspicious25.yml
  git add .github/workflows/suspicious25.yml
  git commit -qm "ordinary: add workflow using a YAML alias for statuses write (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "YAML alias for statuses write fails" 1 "$ec"
  assert_contains "YAML alias for statuses write names the file" "suspicious25.yml" "$out"
  assert_contains "YAML alias for statuses write reports FAIL with the right reason" "introduces a workflow with status/check-write permissions outside thin-fork-guard-trusted.yml" "$out"
}

echo "=== Scenario 26: delete a security-boundary file outright -> FAIL (security-boundary) ==="
{
  dir="$(new_fixture scenario26)"
  cd "$dir" || exit 1
  git checkout -q develop
  mkdir -p scripts
  echo "#!/usr/bin/env bash" >scripts/measure-conflicts.sh
  git add scripts/measure-conflicts.sh
  git commit -qm "develop: placeholder measure-conflicts.sh"
  git checkout -qb head_branch
  git rm -q scripts/measure-conflicts.sh
  git commit -qm "ordinary: delete measure-conflicts.sh (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "delete security-boundary file fails" 1 "$ec"
  assert_contains "delete security-boundary file names it" "scripts/measure-conflicts.sh (modified/deleted)" "$out"
  assert_contains "delete security-boundary file reports FAIL with the right reason" "security-boundary file modified -- explicit guard-maintenance procedure required" "$out"
}

echo "=== Scenario 27: unparseable workflow YAML at PR HEAD -> guard fails closed (hard abort) ==="
{
  dir="$(new_fixture scenario27)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: Broken\non:\n  pull_request_target:\npermissions:\n  statuses: [unterminated\n' >.github/workflows/broken.yml
  git add .github/workflows/broken.yml
  git commit -qm "ordinary: add a malformed/unparseable workflow file"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "malformed workflow YAML fails closed" 1 "$ec"
  assert_contains "malformed workflow YAML reports the audit could not complete" "workflow-permission audit could not complete -- failing closed" "$out"
  assert_contains "malformed workflow YAML names the broken file" "broken.yml" "$out"
}

echo
echo "=== Scenario 28: ordinary PR modifies scripts/test-thin-fork-guard.sh -> FAIL (trusted executable code, not just governance) ==="
{
  dir="$(new_fixture scenario28)"
  cd "$dir" || exit 1
  git checkout -q develop
  mkdir -p scripts
  printf '#!/usr/bin/env bash\necho ok\n' >scripts/test-thin-fork-guard.sh
  git add scripts/test-thin-fork-guard.sh
  git commit -qm "develop: placeholder test-thin-fork-guard.sh"
  git checkout -qb head_branch
  cat >scripts/test-thin-fork-guard.sh <<'MALICIOUS'
#!/usr/bin/env bash
# malicious: if this ran as the trusted evaluator's self-test step (before
# the real guard, from the base checkout), it would overwrite the real
# guard with an always-PASS stub before it ever runs.
echo 'echo PASS; exit 0' > "$(dirname "$0")/thin-fork-guard.sh"
exit 0
MALICIOUS
  git add scripts/test-thin-fork-guard.sh
  git commit -qm "ordinary: modify test-thin-fork-guard.sh to tamper with the real guard (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "modify test-thin-fork-guard.sh fails" 1 "$ec"
  assert_contains "modify test-thin-fork-guard.sh names it" "scripts/test-thin-fork-guard.sh (modified/deleted)" "$out"
  assert_contains "modify test-thin-fork-guard.sh reports FAIL with the right reason" "security-boundary file modified -- explicit guard-maintenance procedure required" "$out"
}

echo "=== Scenario 29: new pull_request_target workflow with no permissions block -> FAIL ==="
{
  dir="$(new_fixture scenario29)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: NoPerms\non: pull_request_target\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps: []\n' >.github/workflows/noperms29.yml
  git add .github/workflows/noperms29.yml
  git commit -qm "ordinary: add pull_request_target workflow with no permissions block (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "pull_request_target with no permissions fails" 1 "$ec"
  assert_contains "pull_request_target with no permissions names the file" "noperms29.yml" "$out"
  assert_contains "pull_request_target with no permissions reports the specific reason" "pull_request_target workflow has no explicit top-level permissions declaration" "$out"
}

echo "=== Scenario 30: pull_request_target with explicit read-only permissions -> PASS ==="
{
  dir="$(new_fixture scenario30)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: ReadOnly\non: pull_request_target\npermissions:\n  contents: read\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps: []\n' >.github/workflows/readonly30.yml
  git add .github/workflows/readonly30.yml
  git commit -qm "ordinary: add pull_request_target workflow with explicit read-only permissions"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "pull_request_target with explicit read-only permissions passes" 0 "$ec"
  assert_contains "pull_request_target with explicit read-only permissions reports PASS" "PASS" "$out"
}

echo "=== Scenario 31: pull_request_target with explicit permissions but statuses: write -> FAIL (existing forbidden-permission rule) ==="
{
  dir="$(new_fixture scenario31)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: BadPerms\non: pull_request_target\npermissions:\n  contents: read\n  statuses: write\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps: []\n' >.github/workflows/badperms31.yml
  git add .github/workflows/badperms31.yml
  git commit -qm "ordinary: add pull_request_target workflow with explicit statuses: write (should be blocked)"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "pull_request_target with explicit statuses write fails" 1 "$ec"
  assert_contains "pull_request_target with explicit statuses write names the file" "badperms31.yml" "$out"
  assert_contains "pull_request_target with explicit statuses write reports the forbidden-permission reason (not the missing-declaration one)" "introduces a workflow with status/check-write permissions outside thin-fork-guard-trusted.yml" "$out"
}

echo "=== Scenario 32: mapping-form and array-form pull_request_target triggers with explicit read-only permissions -> PASS ==="
{
  dir="$(new_fixture scenario32)"
  cd "$dir" || exit 1
  git checkout -q develop
  git checkout -qb head_branch
  mkdir -p .github/workflows
  printf 'name: MappingForm\non:\n  pull_request_target:\n    types: [opened, reopened, edited]\npermissions:\n  pull-requests: read\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps: []\n' >.github/workflows/mapping32.yml
  printf 'name: ArrayForm\non: [push, pull_request_target]\npermissions:\n  contents: read\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps: []\n' >.github/workflows/array32.yml
  git add .github/workflows/mapping32.yml .github/workflows/array32.yml
  git commit -qm "ordinary: add mapping-form and array-form pull_request_target workflows, both with explicit read-only permissions"
  out="$("$GUARD" develop head_branch upstream_mirror 2>&1)"
  ec=$?
  assert_exit "mapping-form and array-form pull_request_target with explicit permissions passes" 0 "$ec"
  assert_contains "mapping-form and array-form pull_request_target reports PASS" "PASS" "$out"
}

echo
echo "=== $TESTS_RUN assertions, $FAILURES failed ==="
if [ "$FAILURES" -ne 0 ]; then
  exit 1
fi
exit 0
# validation-b: tamper with the self-test itself
