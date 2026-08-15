#!/usr/bin/env bash
# test-classify-crosspoint-sync.sh
#
# Self-contained tests for classify-crosspoint-sync.sh. Builds tiny
# synthetic git repos per scenario (not real CrossPoint/Midad content) and
# asserts the last `CLASS=` line + key report content. No network access,
# no dependency on this repo's own history.
#
# Scenario 1: harmless file, no sensitive hits -> Class A.
# Scenario 2: a hardware_sdk-category path -> Class B.
# Scenario 3: a .github/** touch -> Class B via the hard-coded rule, even
#             though .github/** is not itself a YAML category.
# Scenario 4: a freeink-sdk gitlink-shaped path -> Class B via the other
#             hard-coded rule.
# Scenario 5: a rename INTO a sensitive category -> Class B (new path
#             checked, not just old path).
# Scenario 6: a rename OUT OF a sensitive category to a harmless path ->
#             still Class B (old path checked too -- either direction
#             trips the gate, matching thin-fork-guard.sh's own reasoning).
# Scenario 7: no files touched at all -> Class A, reported explicitly.
# Scenario 8: missing sensitive-paths config file -> fails closed (exit 1).
# Scenario 9: a reader_architecture path -> Class B (Phase 2 not complete).
# Scenario 10: multiple sensitive categories touched in one merge -> both
#              categories listed, still just Class B (not a distinct
#              "worse" class -- B is B).
#
# Usage: scripts/test-classify-crosspoint-sync.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLASSIFIER="$SCRIPT_DIR/classify-crosspoint-sync.sh"
WORKDIR="$(mktemp -d)"
FAILURES=0
TESTS_RUN=0

# shellcheck disable=SC2329 # invoked indirectly via `trap ... EXIT` below
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

pass() { echo "  ok: $1"; }
fail() { echo "  FAIL: $1"; FAILURES=$((FAILURES + 1)); }

assert_class() {
  local desc="$1" expected="$2" output="$3"
  local actual
  actual="$(printf '%s\n' "$output" | grep -E '^CLASS=' | tail -1)"
  TESTS_RUN=$((TESTS_RUN + 1))
  if [ "$actual" = "CLASS=$expected" ]; then
    pass "$desc ($actual)"
  else
    fail "$desc (expected CLASS=$expected, got '$actual')"
  fi
}

assert_contains() {
  local desc="$1" needle="$2" haystack="$3"
  TESTS_RUN=$((TESTS_RUN + 1))
  if grep -qF "$needle" <<<"$haystack"; then
    pass "$desc"
  else
    fail "$desc (did not find: $needle)"
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

# Builds a fresh repo at $1 with a real sensitive-paths config (mirroring
# .github/crosspoint-sensitive-paths.yml's categories at a reduced scale --
# not a copy of the real file, so this test suite doesn't silently pass or
# fail just because the real config later grows/shrinks). Returns the repo
# path's sensitive-paths file path on stdout.
new_fixture() {
  local dir="$1"
  mkdir -p "$dir"
  git -C "$dir" init -q
  git -C "$dir" config user.name "test"
  git -C "$dir" config user.email "test@example.com"

  mkdir -p "$dir/.github"
  cat >"$dir/.github/sensitive-paths.yml" <<'EOF'
hardware_sdk:
  - "lib/hal/**"
  - "platformio.ini"

reader_architecture:
  - "src/activities/reader/**"
EOF

  echo "harmless" >"$dir/README.md"
  git -C "$dir" add -A
  git -C "$dir" commit -q -m "base"
}

echo "=== classify-crosspoint-sync.sh test suite ==="
echo

echo "--- Scenario 1: harmless file only -> Class A ---"
repo="$WORKDIR/s1"
new_fixture "$repo"
git -C "$repo" branch base_ref
echo "changed" >"$repo/README.md"
git -C "$repo" add -A
git -C "$repo" commit -q -m "head"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class "harmless-only diff" "A" "$output"
assert_contains "no sensitive hits reported" "No sensitive-path hits." "$output"
echo

echo "--- Scenario 2: hardware_sdk path -> Class B ---"
repo="$WORKDIR/s2"
new_fixture "$repo"
git -C "$repo" branch base_ref
mkdir -p "$repo/lib/hal"
echo "x" >"$repo/lib/hal/HalDisplay.h"
git -C "$repo" add -A
git -C "$repo" commit -q -m "head"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class "hardware_sdk touch" "B" "$output"
assert_contains "category named in report" "hardware_sdk" "$output"
echo

echo "--- Scenario 3: .github/** touch -> Class B (hard-coded rule) ---"
repo="$WORKDIR/s3"
new_fixture "$repo"
git -C "$repo" branch base_ref
mkdir -p "$repo/.github/workflows"
echo "x" >"$repo/.github/workflows/new.yml"
git -C "$repo" add -A
git -C "$repo" commit -q -m "head"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class ".github/** touch" "B" "$output"
assert_contains "hardcoded category named" "hardcoded" "$output"
echo

echo "--- Scenario 4: freeink-sdk gitlink path -> Class B (hard-coded rule) ---"
repo="$WORKDIR/s4"
new_fixture "$repo"
git -C "$repo" branch base_ref
echo "x" >"$repo/freeink-sdk"
git -C "$repo" add -A
git -C "$repo" commit -q -m "head"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class "freeink-sdk path touch" "B" "$output"
echo

echo "--- Scenario 5: rename INTO a sensitive category -> Class B ---"
repo="$WORKDIR/s5"
new_fixture "$repo"
mkdir -p "$repo/src"
echo "some content that is long enough to be detected as a rename by git, not a delete+add pair, needs real similarity" >"$repo/src/original.cpp"
git -C "$repo" add -A
git -C "$repo" commit -q -m "add original"
git -C "$repo" branch base_ref
mkdir -p "$repo/lib/hal"
git -C "$repo" mv src/original.cpp lib/hal/renamed.cpp
git -C "$repo" commit -q -m "head"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class "rename into lib/hal/**" "B" "$output"
echo

echo "--- Scenario 6: rename OUT OF a sensitive category -> still Class B ---"
repo="$WORKDIR/s6"
new_fixture "$repo"
mkdir -p "$repo/lib/hal"
echo "some content that is long enough to be detected as a rename by git, not a delete+add pair, needs real similarity" >"$repo/lib/hal/original.cpp"
git -C "$repo" add -A
git -C "$repo" commit -q -m "add original"
git -C "$repo" branch base_ref
git -C "$repo" mv lib/hal/original.cpp README2.md
git -C "$repo" commit -q -m "head"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class "rename out of lib/hal/**" "B" "$output"
echo

echo "--- Scenario 7: no files touched -> Class A, reported explicitly ---"
repo="$WORKDIR/s7"
new_fixture "$repo"
git -C "$repo" branch base_ref
git -C "$repo" commit -q --allow-empty -m "head (no file changes)"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class "empty diff" "A" "$output"
assert_contains "explicit zero-touch report" "nothing to classify" "$output"
echo

echo "--- Scenario 8: missing sensitive-paths config -> fails closed ---"
repo="$WORKDIR/s8"
new_fixture "$repo"
git -C "$repo" branch base_ref
echo "changed" >"$repo/README.md"
git -C "$repo" add -A
git -C "$repo" commit -q -m "head"
set +e
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/does-not-exist.yml" 2>&1)"
exit_code=$?
set -e
assert_exit "missing config fails closed" "1" "$exit_code"
assert_contains "failing-closed message present" "failing closed" "$output"
echo

echo "--- Scenario 9: reader_architecture path -> Class B ---"
repo="$WORKDIR/s9"
new_fixture "$repo"
git -C "$repo" branch base_ref
mkdir -p "$repo/src/activities/reader"
echo "x" >"$repo/src/activities/reader/ReaderActivity.cpp"
git -C "$repo" add -A
git -C "$repo" commit -q -m "head"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class "reader_architecture touch" "B" "$output"
assert_contains "category named in report" "reader_architecture" "$output"
echo

echo "--- Scenario 10: multiple categories touched -> both listed, Class B ---"
repo="$WORKDIR/s10"
new_fixture "$repo"
git -C "$repo" branch base_ref
mkdir -p "$repo/lib/hal" "$repo/src/activities/reader"
echo "x" >"$repo/lib/hal/HalDisplay.h"
echo "x" >"$repo/src/activities/reader/ReaderActivity.cpp"
git -C "$repo" add -A
git -C "$repo" commit -q -m "head"
output="$(cd "$repo" && "$CLASSIFIER" base_ref HEAD "$repo/.github/sensitive-paths.yml" 2>&1)"
assert_class "two categories touched" "B" "$output"
assert_contains "hardware_sdk listed" "hardware_sdk" "$output"
assert_contains "reader_architecture listed" "reader_architecture" "$output"
echo

echo "=== Results: $((TESTS_RUN - FAILURES))/$TESTS_RUN passed ==="
if [ "$FAILURES" -gt 0 ]; then
  exit 1
fi
exit 0
