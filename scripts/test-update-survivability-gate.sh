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

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASSED=0
FAILED=0

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

# run_case <name> <expected-exit> <changed-files> <evidence> <labels> <title>
run_case() {
  local name="$1" want="$2" changed="$3" evidence="$4" labels="$5" title="$6"
  local cf="$TMP/changed" ev="$TMP/evidence" lb="$TMP/labels" out="$TMP/out" got=0

  printf '%s\n' "$changed" >"$cf"
  printf '%s\n' "$evidence" >"$ev"
  printf '%s' "$labels" >"$lb"

  set +e
  "$GATE" "$cf" "$ev" "$lb" "$title" >"$out" 2>&1
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

# expect_output <name> <grep-ere> <changed-files> <evidence> <labels> <title>
expect_output() {
  local name="$1" pattern="$2" changed="$3" evidence="$4" labels="$5" title="$6"
  local cf="$TMP/changed" ev="$TMP/evidence" lb="$TMP/labels" out="$TMP/out"

  printf '%s\n' "$changed" >"$cf"
  printf '%s\n' "$evidence" >"$ev"
  printf '%s' "$labels" >"$lb"

  set +e
  "$GATE" "$cf" "$ev" "$lb" "$title" >"$out" 2>&1
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

set +e
"$GATE" "$TMP/does-not-exist" "$TMP/evidence" "$TMP/labels" "t" >"$TMP/out" 2>&1
rc=$?
set -e
if [ "$rc" -eq 2 ]; then
  echo "  ok   unreadable changed-file list (exit 2)"
  PASSED=$((PASSED + 1))
else
  echo "  FAIL unreadable changed-file list: expected exit 2, got $rc"
  FAILED=$((FAILED + 1))
fi

set +e
"$GATE" "$TMP/changed" "$TMP/evidence" >"$TMP/out" 2>&1
rc=$?
set -e
if [ "$rc" -eq 2 ]; then
  echo "  ok   wrong argument count (exit 2)"
  PASSED=$((PASSED + 1))
else
  echo "  FAIL wrong argument count: expected exit 2, got $rc"
  FAILED=$((FAILED + 1))
fi

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
echo "-------------------------------------------"
echo "passed: $PASSED   failed: $FAILED"
if [ "$FAILED" -ne 0 ]; then
  echo "test-update-survivability-gate.sh: FAILED"
  exit 1
fi
echo "test-update-survivability-gate.sh: OK"
