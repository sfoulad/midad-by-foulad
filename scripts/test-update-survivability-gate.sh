#!/usr/bin/env bash
#
# test-update-survivability-gate.sh
#
# Self-test for scripts/update-survivability-gate.sh, run by
# .github/workflows/update-survivability-gate.yml BEFORE the gate's verdict on
# a real PR is trusted. Synthetic fixtures only -- no dependency on this
# repository's real history, so the test means the same thing on every branch.
#
# Mirrors scripts/test-thin-fork-guard.sh's role for the thin-fork guard: a
# gate whose own matching logic has never been exercised is not evidence.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="$SCRIPT_DIR/update-survivability-gate.sh"
GATE_DOC_FILE="$SCRIPT_DIR/../docs/update-survivability-gate.md"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASSED=0
FAILED=0

# The PR head SHA every fixture is judged against. A maintainer approval only
# clears a gate self-modification when it was recorded against THIS commit.
HEAD_SHA="cafebabecafebabecafebabecafebabecafebabe"
OLD_HEAD_SHA="0ff5e70ff5e70ff5e70ff5e70ff5e70ff5e70ff5"

# Review fixtures, one `state<TAB>author_association<TAB>commit_id` per line --
# the shape the workflow produces from the GitHub pull-reviews API.
REVIEW_NONE=""
REVIEW_OWNER_CURRENT="APPROVED\tOWNER\t$HEAD_SHA"
REVIEW_MEMBER_CURRENT="APPROVED\tMEMBER\t$HEAD_SHA"
REVIEW_COLLABORATOR_CURRENT="APPROVED\tCOLLABORATOR\t$HEAD_SHA"
REVIEW_OWNER_STALE="APPROVED\tOWNER\t$OLD_HEAD_SHA"
REVIEW_OUTSIDER_CURRENT="APPROVED\tCONTRIBUTOR\t$HEAD_SHA"
REVIEW_OWNER_COMMENTED="COMMENTED\tOWNER\t$HEAD_SHA"

# A complete, well-formed report. Individual tests mutate copies of it.
complete_report() {
  cat <<'REPORT'
## Hardware validation

Test device: X4 Pro #2 (designated test unit, MAC 38:44:be:e1:d8:41)
Build SHAs: A=3321f1e1 B=a1b2c3d4e5f C=9f8e7d6

### Survivability matrix

| Case  | Running | Target | Image SHA-256 | Boot | Verdict |
|-------|---------|--------|---------------|------|---------|
| US-0  | app0    | -      | -             | -    | PASS |
| US-1  | app0    | app1   | 1a2b...       | ok   | PASS |
| US-2  | app1    | app0   | 3c4d...       | ok   | PASS |
| US-3  | app0    | app1   | 5e6f...       | ok   | PASS |
| US-4  | app1    | app1   | -             | -    | N/A  |
| US-5  | app1    | -      | 7a8b...       | ok   | PASS |
| US-6  | app1    | -      | 9c0d...       | ok   | PASS |
| US-7  | app1    | -      | e1f2...       | ok   | PASS |
| US-8  | app1    | -      | -             | ok   | PASS |
| US-9  | app1    | -      | -             | ok   | PASS |
| US-10 | app1    | app0   | 3344...       | ok   | PASS |
| US-11 | app1    | -      | -             | ok   | PASS |
REPORT
}

# write_fixtures <changed> <evidence> <labels> <reviews>
write_fixtures() {
  printf '%s\n' "$1" >"$TMP/changed"
  printf '%s\n' "$2" >"$TMP/evidence"
  printf '%s' "$3" >"$TMP/labels"
  printf '%b\n' "$4" >"$TMP/reviews"
}

# run_case <name> <expected-exit> <changed-files> <evidence> <labels> <title> \
#          [reviews] [head-sha]
run_case() {
  local name="$1" want="$2" changed="$3" evidence="$4" labels="$5" title="$6"
  local reviews="${7-}" head="${8-$HEAD_SHA}"
  local cf="$TMP/changed" ev="$TMP/evidence" lb="$TMP/labels" rv="$TMP/reviews"
  local out="$TMP/out" got=0

  write_fixtures "$changed" "$evidence" "$labels" "$reviews"

  set +e
  "$GATE" "$cf" "$ev" "$lb" "$title" "$rv" "$head" >"$out" 2>&1
  got=$?
  set -e

  if [ "$got" -eq "$want" ]; then
    echo "  ok   $name (exit $got)"
    PASSED=$((PASSED + 1))
  else
    echo "  FAIL $name: expected exit $want, got $got"
    sed 's/^/       | /' "$out"
    FAILED=$((FAILED + 1))
  fi
}

# expect_output <name> <grep-ere> <changed-files> <evidence> <labels> <title> \
#               [reviews] [head-sha]
expect_output() {
  local name="$1" pattern="$2" changed="$3" evidence="$4" labels="$5" title="$6"
  local reviews="${7-}" head="${8-$HEAD_SHA}"
  local cf="$TMP/changed" ev="$TMP/evidence" lb="$TMP/labels" rv="$TMP/reviews"
  local out="$TMP/out"

  write_fixtures "$changed" "$evidence" "$labels" "$reviews"

  set +e
  "$GATE" "$cf" "$ev" "$lb" "$title" "$rv" "$head" >"$out" 2>&1
  set -e

  if grep -qE "$pattern" "$out"; then
    echo "  ok   $name"
    PASSED=$((PASSED + 1))
  else
    echo "  FAIL $name: output did not match /$pattern/"
    sed 's/^/       | /' "$out"
    FAILED=$((FAILED + 1))
  fi
}

# expect_absent <name> <grep-ere> <changed-files> <evidence> <labels> <title> \
#               [reviews] [head-sha]
expect_absent() {
  local name="$1" pattern="$2" changed="$3" evidence="$4" labels="$5" title="$6"
  local reviews="${7-}" head="${8-$HEAD_SHA}"
  local cf="$TMP/changed" ev="$TMP/evidence" lb="$TMP/labels" rv="$TMP/reviews"
  local out="$TMP/out"

  write_fixtures "$changed" "$evidence" "$labels" "$reviews"

  set +e
  "$GATE" "$cf" "$ev" "$lb" "$title" "$rv" "$head" >"$out" 2>&1
  set -e

  if grep -qE "$pattern" "$out"; then
    echo "  FAIL $name: output unexpectedly matched /$pattern/"
    sed 's/^/       | /' "$out"
    FAILED=$((FAILED + 1))
  else
    echo "  ok   $name"
    PASSED=$((PASSED + 1))
  fi
}

UI_ONLY_DIFF='src/activities/settings/DisplaySettingsActivity.cpp
lib/UITheme/UITheme.cpp
lib/I18n/translations/english.yaml'

UI_PLUS_UPDATER_DIFF='src/activities/settings/DisplaySettingsActivity.cpp
lib/UITheme/UITheme.cpp
src/network/OtaUpdater.cpp
src/network/FirmwareFlasher.cpp'

echo "== negative path: PRs the gate must let through =="
run_case "UI-only diff, UI label, no report" 0 \
  "$UI_ONLY_DIFF" "Adds a touch target to the display settings list." "ui" "feat(ui): bigger touch targets"
run_case "docs-only diff" 0 \
  "docs/troubleshooting.md" "Typo fix." "" "docs: fix typo"
run_case "similarly-named but unprotected file" 0 \
  "src/network/HttpDownloader.cpp" "Retry tweak." "" "fix: retry once on a truncated read"
run_case "carve-out: release-fonts.yml is not firmware release machinery" 0 \
  ".github/workflows/release-fonts.yml" "Bump the font pack." "" "chore: new font pack"
run_case "the carve-out is exact, not a prefix (release.yml still blocks)" 1 \
  ".github/workflows/release.yml" "Bump asset name." "" "chore: rename asset"
run_case "protected path touched WITH a complete report" 0 \
  "$UI_PLUS_UPDATER_DIFF" "$(complete_report)" "ui" "feat(ui): touch install screen"

echo
echo "== positive path: PRs the gate must block =="
run_case "UI PR touching the updater, no report" 1 \
  "$UI_PLUS_UPDATER_DIFF" "Reworks the install screen for touch." "ui" "feat(ui): touch install screen"
run_case "non-UI PR touching the updater, no report (label is irrelevant)" 1 \
  "src/network/OtaUpdater.cpp" "Refactor." "" "refactor: tidy installUpdate"
run_case "partitions.csv" 1 \
  "partitions.csv" "Grow spiffs." "" "chore: repartition"
run_case "release workflow" 1 \
  ".github/workflows/release.yml" "Bump artifact name." "" "chore: rename asset"
run_case "rollback source" 1 \
  "src/OtaRollbackDetection.cpp" "Tidy." "" "refactor: rollback"
run_case "broad pattern catches a NEW updater file the named list predates" 1 \
  "src/network/FirmwareSignature.cpp" "New signature verifier." "" "feat: verify signatures"
run_case "signing script" 1 \
  "scripts/sign_firmware.py" "New signer." "" "feat: sign images"

echo
echo "== renames: BOTH sides of every rename are classified =="
# The GitHub pull-files API reports a rename's destination in `filename` and
# its source in `previous_filename`; the gate's changed-file records carry them
# as `destination<TAB>source`. Matching the destination alone was a total
# bypass: rename src/network/OtaUpdater.cpp to src/network/Updater.cpp, rewrite
# it in the same commit, and the resulting list matches no protected pattern,
# so the gate reports "Not applicable" and the updater ships with no hardware
# validation. Every case below fails loudly if that regresses.
TAB=$'\t'

# Protected SOURCE -> unprotected destination. Contents changed in the same
# commit. This is the reported bypass; it must block.
RENAME_AWAY_DIFF="src/network/Updater.cpp${TAB}src/network/OtaUpdater.cpp
src/network/Updater.h${TAB}src/network/OtaUpdater.h
src/activities/settings/DisplaySettingsActivity.cpp"

# Unprotected source -> protected DESTINATION. Whatever now lives at the
# protected pathname is the update path from this commit on.
RENAME_INTO_DIFF="src/network/OtaUpdater.cpp${TAB}src/network/Updater.cpp
lib/UITheme/UITheme.cpp"

run_case "protected source renamed to unprotected destination, contents changed" 1 \
  "$RENAME_AWAY_DIFF" "Renames the updater while reworking the install screen." "ui" \
  "feat(ui): touch install screen"
run_case "unprotected source renamed INTO a protected path" 1 \
  "$RENAME_INTO_DIFF" "Moves the download helper." "" "refactor: rename Updater to OtaUpdater"
run_case "unprotected renamed to unprotected" 0 \
  "src/network/HttpClient.cpp${TAB}src/network/HttpDownloader.cpp" "Rename only." "" \
  "refactor: rename the downloader"
# The gate has no diff-size, patch or similarity-score input at all: a pure
# 100%-similarity rename that changes not one byte is classified exactly like a
# rewrite, because the question is which paths moved, never how much moved.
run_case "pure rename, no content change, is still classified by path" 1 \
  "src/network/Updater.cpp${TAB}src/network/OtaUpdater.cpp" \
  "Pure rename, zero content change." "" "refactor: shorter filename"
run_case "renamed protected path WITH a complete report" 0 \
  "$RENAME_AWAY_DIFF" "$(complete_report)" "ui" "feat(ui): touch install screen"
run_case "both sides protected (OtaUpdater -> FirmwareFlasher-ish)" 1 \
  "src/network/OtaBootSwitch.cpp${TAB}src/network/OtaUpdater.cpp" "Split." "" "refactor: split ota"

# Exclusions are evaluated per PATH, not per record, so the one documented
# carve-out cannot be used as a laundering route in either direction.
run_case "carve-out source renamed INTO real release machinery" 1 \
  ".github/workflows/release-firmware.yml${TAB}.github/workflows/release-fonts.yml" \
  "Repurpose the font workflow." "" "chore: reuse the workflow"
run_case "carve-out renamed to something unprotected" 0 \
  "docs/font-packs.md${TAB}.github/workflows/release-fonts.yml" "Retire the workflow." "" \
  "chore: drop the font workflow"
run_case "protected path laundered through the carve-out name" 1 \
  ".github/workflows/release-fonts.yml${TAB}.github/workflows/release.yml" "Rename." "" \
  "chore: rename release workflow"

# A record with an empty second field is an ordinary non-rename change, so a
# plain one-path-per-line list (every other test above) stays valid input.
run_case "empty rename field is a plain change (protected)" 1 \
  "src/network/OtaUpdater.cpp${TAB}" "No report." "" "fix: ota"
run_case "empty rename field is a plain change (unprotected)" 0 \
  "docs/troubleshooting.md${TAB}" "Typo." "" "docs: typo"

echo
echo "== rename message quality: name BOTH paths and the direction =="
expect_output "rename-away failure names the source path the author no longer has" \
  'src/network/OtaUpdater\.cpp -> src/network/Updater\.cpp' \
  "$RENAME_AWAY_DIFF" "no report" "" "refactor: rename the updater"
expect_output "rename-away failure says it matched on the SOURCE path" \
  'RENAMED AWAY from a protected path' \
  "$RENAME_AWAY_DIFF" "no report" "" "refactor: rename the updater"
expect_output "rename-into failure names both paths" \
  'src/network/Updater\.cpp -> src/network/OtaUpdater\.cpp' \
  "$RENAME_INTO_DIFF" "no report" "" "refactor: rename Updater to OtaUpdater"
expect_output "rename-into failure says it matched on the DESTINATION path" \
  'RENAMED INTO a protected path' \
  "$RENAME_INTO_DIFF" "no report" "" "refactor: rename Updater to OtaUpdater"
expect_output "the failure explains why a path not in the diff is blocking" \
  'A rename is what put a protected path in this list' \
  "$RENAME_AWAY_DIFF" "no report" "" "refactor: rename the updater"
expect_absent "a non-rename failure carries no rename explanation" \
  'RENAMED|A rename is what put' \
  "src/network/OtaUpdater.cpp" "no report" "" "fix: ota"
expect_absent "an unprotected rename is not reported as touching a protected path" \
  'Blocked|Protected files touched' \
  "src/network/HttpClient.cpp${TAB}src/network/HttpDownloader.cpp" "no report" "" "refactor: rename"

echo
echo "== report completeness =="
run_case "matrix row missing (US-11 deleted)" 1 \
  "src/network/OtaUpdater.cpp" "$(complete_report | grep -v 'US-11')" "" "fix: ota"
run_case "matrix row recorded as FAIL" 1 \
  "src/network/OtaUpdater.cpp" "$(complete_report | sed 's/| US-7 .*|.*| PASS |/| US-7  | app1 | - | e1f2 | ok | FAIL |/')" "" "fix: ota"
run_case "matrix row with no verdict at all" 1 \
  "src/network/OtaUpdater.cpp" "$(complete_report | sed 's/| US-5 \(.*\)PASS |/| US-5 \1|/')" "" "fix: ota"
run_case "test device not named" 1 \
  "src/network/OtaUpdater.cpp" "$(complete_report | grep -v '^Test device:')" "" "fix: ota"
run_case "build SHAs are a TBD placeholder, not hex" 1 \
  "src/network/OtaUpdater.cpp" "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: TBD/')" "" "fix: ota"
run_case "matrix block not labelled as such" 1 \
  "src/network/OtaUpdater.cpp" "$(complete_report | sed 's/Survivability matrix/Results/')" "" "fix: ota"

echo
echo "== id anchoring: US-1 must not be satisfied by US-10 or US-11 =="
run_case "US-1 removed but US-10/US-11 present" 1 \
  "src/network/OtaUpdater.cpp" "$(complete_report | grep -v '| US-1  ')" "" "fix: ota"
expect_output "the missing-row list names US-1 specifically" '^US-1$' \
  "src/network/OtaUpdater.cpp" "$(complete_report | grep -v '| US-1  ')" "" "fix: ota"

echo
echo "== fail-closed: undetermined inputs must be exit 2, never exit 0 =="
run_case "empty changed-file list" 2 "" "irrelevant" "" "chore: nothing"
run_case "whitespace-only changed-file list" 2 "   " "irrelevant" "" "chore: nothing"

# expect_exit <name> <expected-exit> <argv...>
expect_exit() {
  local name="$1" want="$2"
  shift 2
  local rc=0
  set +e
  "$GATE" "$@" >"$TMP/out" 2>&1
  rc=$?
  set -e
  if [ "$rc" -eq "$want" ]; then
    echo "  ok   $name (exit $rc)"
    PASSED=$((PASSED + 1))
  else
    echo "  FAIL $name: expected exit $want, got $rc"
    sed 's/^/       | /' "$TMP/out"
    FAILED=$((FAILED + 1))
  fi
}

expect_exit "unreadable changed-file list" 2 \
  "$TMP/does-not-exist" "$TMP/evidence" "$TMP/labels" "t" "$TMP/reviews" "$HEAD_SHA"
# An unreadable REVIEWS file is undetermined, not "nobody approved": the
# difference decides whether a gate self-modification is judged at all.
expect_exit "unreadable reviews list" 2 \
  "$TMP/changed" "$TMP/evidence" "$TMP/labels" "t" "$TMP/does-not-exist" "$HEAD_SHA"
# Without a head SHA an approval cannot be bound to a commit, so a stale
# approval would be indistinguishable from a current one.
expect_exit "empty head SHA" 2 \
  "$TMP/changed" "$TMP/evidence" "$TMP/labels" "t" "$TMP/reviews" ""
expect_exit "wrong argument count (2)" 2 "$TMP/changed" "$TMP/evidence"
expect_exit "wrong argument count (4 -- the pre-review signature)" 2 \
  "$TMP/changed" "$TMP/evidence" "$TMP/labels" "t"

echo
echo "== message quality =="
expect_output "failure names the offending files" 'src/network/OtaUpdater\.cpp' \
  "$UI_PLUS_UPDATER_DIFF" "no report here" "ui" "feat(ui): touch install screen"
expect_output "failure points at the gate document" 'docs/update-survivability-gate\.md' \
  "$UI_PLUS_UPDATER_DIFF" "no report here" "ui" "feat(ui): touch install screen"
expect_output "failure says builds/tests/CodeRabbit are not substitutes" 'CodeRabbit' \
  "$UI_PLUS_UPDATER_DIFF" "no report here" "ui" "feat(ui): touch install screen"
expect_output "UI escalation fires on a label" 'detected via: label' \
  "src/network/OtaUpdater.cpp" "no report" "ui" "chore: bump"
expect_output "UI escalation fires on a title scope" 'detected via: title' \
  "src/network/OtaUpdater.cpp" "no report" "" "feat(ui): install screen"
expect_output "UI escalation fires on diff shape alone" 'detected via: diff-shape' \
  "$UI_PLUS_UPDATER_DIFF" "no report" "" "chore: unlabelled work"

echo
echo "== evidence parsing: a result is a structured ROW, not a mention =="
# The looser "any line carrying the id and any verdict word" rule accepted a
# sentence in the PR body as a completed hardware test, and accepted a row with
# no recorded slots, image hash or boot result at all.
run_case "a prose mention of a case id is not a matrix row" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-6 .*/US-6 wrong-board image was rejected: PASS/')" "" "fix: ota"
run_case "row missing the running-slot cell" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-2 .*/| US-2  |         | app0   | 3c4d...       | ok   | PASS |/')" "" "fix: ota"
run_case "row missing the target-slot cell" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-2 .*/| US-2  | app1    |        | 3c4d...       | ok   | PASS |/')" "" "fix: ota"
run_case "row missing the image SHA-256 cell" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-2 .*/| US-2  | app1    | app0   |               | ok   | PASS |/')" "" "fix: ota"
run_case "row missing the boot-result cell" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-2 .*/| US-2  | app1    | app0   | 3c4d...       |      | PASS |/')" "" "fix: ota"
run_case "row whose cells are still TBD is not a filled-in row" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-2 .*/| US-2  | TBD     | TBD    | TBD           | TBD  | PASS |/')" "" "fix: ota"
run_case "row with too few cells to carry a verdict" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-9 .*/| US-9  | app1 | PASS |/')" "" "fix: ota"
run_case "the verdict must be in the Verdict cell, not loose on the line" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-3 .*/| US-3  | app0    | app1   | 5e6f...       | PASS |      |/')" "" "fix: ota"
expect_output "the message explains that a mention is not a row" \
  'must be a full matrix row' \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-6 .*/US-6 was fine: PASS/')" "" "fix: ota"

echo
echo "== exactly one row per case: a duplicate must never let PASS hide FAIL =="
# The reported hazard, precisely: evidence is the PR body concatenated with the
# hardware report, so an author can present a PASS row for a case in the body
# while the attached report records FAIL for the same case. Taking the first
# match would report the RC as validated.
run_case "an earlier PASS cannot hide a later FAIL for the same case" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report)
| US-2  | app1    | app0   | 3c4d5e6f      | ok   | FAIL |" "" "fix: ota"
run_case "a later PASS cannot hide an earlier FAIL for the same case" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-2 \(.*\)PASS |/| US-2 \1FAIL |/')
| US-2  | app1    | app0   | 3c4d5e6f      | ok   | PASS |" "" "fix: ota"
run_case "two identical PASS rows for one case are still ambiguous" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report)
| US-2  | app1    | app0   | 3c4d5e6f      | ok   | PASS |" "" "fix: ota"
expect_output "the message says exactly one row is required" \
  'exactly one is required' \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report)
| US-2  | app1    | app0   | 3c4d5e6f      | ok   | FAIL |" "" "fix: ota"

echo
echo "== N/A is permitted only for US-4 =="
run_case "N/A on US-4 is accepted" 0 \
  "src/network/OtaUpdater.cpp" "$(complete_report)" "" "fix: ota"
run_case "N/A on US-5 is rejected" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-5 \(.*\)PASS |/| US-5 \1N\/A  |/')" "" "fix: ota"
run_case "N/A on US-11 is rejected" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-11 \(.*\)PASS |/| US-11 \1N\/A  |/')" "" "fix: ota"
run_case "N/A on US-0 (the recovery precondition) is rejected" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-0 \(.*\)PASS |/| US-0 \1N\/A  |/')" "" "fix: ota"
expect_output "the rejection says N/A is not permitted for that case" \
  'N/A is not permitted for this case' \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^| US-5 \(.*\)PASS |/| US-5 \1N\/A  |/')" "" "fix: ota"

echo
echo "== build SHAs: three real, DISTINCT hashes =="
run_case "a single build SHA is not three" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: 3321f1e1/')" "" "fix: ota"
run_case "A and B present, C absent" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: A=3321f1e1 B=a1b2c3d4e5f/')" "" "fix: ota"
run_case "C is a placeholder, not a hash" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: A=3321f1e1 B=a1b2c3d4e5f C=TBD/')" "" "fix: ota"
run_case "a hash shorter than 7 hex is not a commit id" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: A=3321f1e1 B=a1b2c3d4e5f C=9f8e/')" "" "fix: ota"
run_case "A, B and C all identical" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: A=3321f1e1 B=3321f1e1 C=3321f1e1/')" "" "fix: ota"
run_case "B and C identical (C must be distinguishable from B)" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: A=3321f1e1 B=a1b2c3d4e5f C=a1b2c3d4e5f/')" "" "fix: ota"
run_case "case differences do not make two SHAs distinct" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: A=3321f1e1 B=A1B2C3D4E5F C=a1b2c3d4e5f/')" "" "fix: ota"
run_case "three distinct hashes are accepted" 0 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: A=deadbee1 B=deadbee2 C=deadbee3/')" "" "fix: ota"
expect_output "the message asks for three DISTINCT builds" 'THREE DISTINCT' \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Build SHAs:.*/Build SHAs: A=3321f1e1 B=3321f1e1 C=3321f1e1/')" "" "fix: ota"
run_case "a placeholder test device is not a named unit" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(complete_report | sed 's/^Test device:.*/Test device: <model, serial\/MAC>/')" "" "fix: ota"

echo
echo "== gate self-modification: a separate, fail-closed classification =="
# A PR that changes ONLY the gate's own workflow or evaluator matches no
# protected firmware path. Without this classification it reports "Not
# applicable" and an edit weakening the evaluator for every future PR merges
# behind a green check.
GATE_WORKFLOW=".github/workflows/update-survivability-gate.yml"
GATE_SCRIPT="scripts/update-survivability-gate.sh"
GATE_SELFTEST="scripts/test-update-survivability-gate.sh"
GATE_DOC_PATH="docs/update-survivability-gate.md"

run_case "the gate workflow alone blocks" 1 \
  "$GATE_WORKFLOW" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_NONE"
run_case "the evaluator script alone blocks" 1 \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_NONE"
run_case "the gate self-test suite alone blocks" 1 \
  "$GATE_SELFTEST" "Add a test." "" "test: gate" "$REVIEW_NONE"
run_case "the gate document alone blocks" 1 \
  "$GATE_DOC_PATH" "Fix a typo." "" "docs: gate typo" "$REVIEW_NONE"
expect_absent "a gate self-modification is never reported as Not applicable" \
  ':white_check_mark: \*\*Not applicable' \
  "$GATE_WORKFLOW" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_NONE"
expect_output "the failure names the gate file that was touched" \
  '\.github/workflows/update-survivability-gate\.yml' \
  "$GATE_WORKFLOW" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_NONE"
expect_output "the failure says a hardware matrix does not clear it" \
  'says nothing about a change to a CI workflow or a shell script' \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_NONE"
expect_output "the failure states it is a visibility control, not prevention" \
  'visibility control, not a prevention control' \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_NONE"
expect_output "the failure names no approving maintainer review" \
  'no approving maintainer review' \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_NONE"

run_case "an APPROVED OWNER review on the current head clears it" 0 \
  "$GATE_WORKFLOW" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_OWNER_CURRENT"
run_case "an APPROVED MEMBER review on the current head clears it" 0 \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_MEMBER_CURRENT"
run_case "an APPROVED COLLABORATOR review on the current head clears it" 0 \
  "$GATE_DOC_PATH" "Fix a typo." "" "docs: gate typo" "$REVIEW_COLLABORATOR_CURRENT"
run_case "an approval recorded against an EARLIER commit does not clear it" 1 \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_OWNER_STALE"
expect_output "a stale approval is reported as given against an earlier commit" \
  'earlier commit' \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_OWNER_STALE"
run_case "an approval from a non-maintainer association does not clear it" 1 \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_OUTSIDER_CURRENT"
expect_output "a non-maintainer approval is reported as an association problem" \
  'author_association' \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_OUTSIDER_CURRENT"
run_case "a COMMENTED review from the owner is not an approval" 1 \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_OWNER_COMMENTED"
run_case "no review at all blocks" 1 \
  "$GATE_SCRIPT" "Tighten the gate." "" "ci: gate tweak" "$REVIEW_NONE"

# The clearing mechanism must not be anything the PR author writes: the same
# person who wrote the change writes the body, the title and the labels.
run_case "an acknowledgement line in the PR body does NOT clear it" 1 \
  "$GATE_SCRIPT" "Gate-self-modification: tightening the parser, reviewed by me." \
  "" "ci: gate tweak" "$REVIEW_NONE"
run_case "a label does NOT clear it" 1 \
  "$GATE_SCRIPT" "Tighten the gate." "gate-self-modification" "ci: gate tweak" "$REVIEW_NONE"

run_case "renaming the evaluator away is still a gate self-modification" 1 \
  "scripts/us-gate.sh${TAB}${GATE_SCRIPT}" "Rename." "" "refactor: shorter name" "$REVIEW_NONE"
run_case "renaming something INTO the evaluator path is a gate self-modification" 1 \
  "${GATE_SCRIPT}${TAB}scripts/us-gate.sh" "Rename." "" "refactor: longer name" "$REVIEW_NONE"

# Both classifications can apply at once, and both must then be satisfied.
GATE_PLUS_UPDATER_DIFF="$GATE_WORKFLOW
src/network/OtaUpdater.cpp"
run_case "gate file + protected path: a complete report alone is not enough" 1 \
  "$GATE_PLUS_UPDATER_DIFF" "$(complete_report)" "" "fix: ota and gate" "$REVIEW_NONE"
run_case "gate file + protected path: an approval alone is not enough" 1 \
  "$GATE_PLUS_UPDATER_DIFF" "No report." "" "fix: ota and gate" "$REVIEW_OWNER_CURRENT"
run_case "gate file + protected path: report AND approval passes" 0 \
  "$GATE_PLUS_UPDATER_DIFF" "$(complete_report)" "" "fix: ota and gate" "$REVIEW_OWNER_CURRENT"
run_case "an ordinary PR is unaffected by the reviews input" 0 \
  "docs/troubleshooting.md" "Typo." "" "docs: typo" "$REVIEW_NONE"

echo
echo "== lockstep: the document's own §5 template must satisfy this parser =="
# The single most important test here. Tightening the parser without updating
# the published template would reject every real hardware report a human fills
# in, which makes the gate unusable and therefore routed around. These two
# cases pin the template and the parser to each other: the REAL template, read
# out of docs/update-survivability-gate.md, must pass when filled in and must
# fail when left as issued.

# The fenced markdown block inside "## 5. Results table".
doc_results_template() {
  # The fence is toggled BEFORE the section-end test: the template itself
  # contains `## Hardware validation`, which would otherwise be read as the
  # start of the next document section.
  awk '
    /^## 5\./ { in5 = 1; next }
    in5 && /^```/ { fence = 1 - fence; next }
    in5 && !fence && /^## / { in5 = 0 }
    in5 && fence { print }
  ' "$GATE_DOC_FILE"
}

# Fill that template in the way a tester would: every placeholder line gets a
# plausible value, and every EMPTY matrix cell is filled according to its own
# column heading -- so reordering, renaming or adding a column in the document
# changes what this produces and breaks the test rather than silently drifting.
fill_doc_template() {
  doc_results_template | awk '
    function trim(v) { gsub(/^[[:space:]]+|[[:space:]]+$/, "", v); return v }
    /^Test device:/ { print "Test device: X4 Pro #2 (designated test unit, MAC 38:44:be:e1:d8:41)"; next }
    /^Recovery method proven:/ { print "Recovery method proven: tested rollback"; next }
    /^Build SHAs:/ { print "Build SHAs: A=3321f1e1 B=a1b2c3d4e5f C=9f8e7d6"; next }
    /^Image SHA-256:/ { print "Image SHA-256: A=aa11bb22cc33 B=dd44ee55ff66 C=1122334455aa"; next }
    /^Firmware channel/ { print "Firmware channel(s) exercised: both"; next }
    /^[[:space:]]*\|/ {
      n = split($0, c, "|")
      id = trim(c[2])
      if (tolower(id) == "id") {
        for (i = 2; i < n; i++) hdr[i] = tolower(trim(c[i]))
        print
        next
      }
      if (id ~ /^-+$/) { print; next }
      if (id ~ /^US-/) {
        out = ""
        for (i = 2; i < n; i++) {
          v = trim(c[i])
          if (v == "" && i > 2) {
            h = hdr[i]
            if (h ~ /running/) v = "app0"
            else if (h ~ /target/) v = "app1"
            else if (h ~ /sha/) v = "1a2b3c4d5e6f7a8b"
            else if (h ~ /boot/) v = "booted ok"
            else if (h ~ /verdict/) v = "PASS"
            else if (h ~ /note/) v = ""
            else v = "-"
          }
          out = out "| " v " "
        }
        print out "|"
        next
      }
      print
      next
    }
    { print }
  '
}

DOC_TEMPLATE_RAW="$(doc_results_template)"
DOC_TEMPLATE_FILLED="$(fill_doc_template)"

if [ -n "$DOC_TEMPLATE_RAW" ] && grep -q 'US-11' <<<"$DOC_TEMPLATE_RAW"; then
  echo "  ok   the §5 results template was found in $GATE_DOC_FILE"
  PASSED=$((PASSED + 1))
else
  echo "  FAIL could not extract the §5 results template from $GATE_DOC_FILE"
  FAILED=$((FAILED + 1))
fi

run_case "the document's own §5 template, filled in, PASSES this parser" 0 \
  "src/network/OtaUpdater.cpp" "$DOC_TEMPLATE_FILLED" "" "fix: ota"
run_case "the same template left as issued (blank / <placeholder>) FAILS" 1 \
  "src/network/OtaUpdater.cpp" "$DOC_TEMPLATE_RAW" "" "fix: ota"
run_case "the filled template with an unauthorized N/A still FAILS" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(printf '%s\n' "$DOC_TEMPLATE_FILLED" | sed 's/^| US-7 \(.*\)| PASS |/| US-7 \1| N\/A  |/')" \
  "" "fix: ota"
run_case "the filled template with TBD build SHAs still FAILS" 1 \
  "src/network/OtaUpdater.cpp" \
  "$(printf '%s\n' "$DOC_TEMPLATE_FILLED" | sed 's/^Build SHAs:.*/Build SHAs: TBD/')" \
  "" "fix: ota"

echo
echo "-------------------------------------------"
echo "passed: $PASSED   failed: $FAILED"
if [ "$FAILED" -ne 0 ]; then
  echo "test-update-survivability-gate.sh: FAILED"
  exit 1
fi
echo "test-update-survivability-gate.sh: OK"
